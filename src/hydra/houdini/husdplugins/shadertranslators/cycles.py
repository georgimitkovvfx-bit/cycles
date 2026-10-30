# SPDX-FileCopyrightText: 2026 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

"""Cycles shader translator (Houdini loads husdplugins/shadertranslators/*.py from HOUDINI_PATH).

Translates the Cycles shader nodes (render mask "cycles") like Houdini's default translator,
except for the parameters that shader2hda.py lays out like Blender: ramp parameters (tag
cycles_socket) are baked to the Cycles lookup table socket they edit, sampled at 256 positions
like Blender. The channels of RGB and Vector Curves (tag cycles_channel r/g/b/c or x/y/z) are
combined into one table, RGB Curves applying the combined curve "c" first like Blender.
Parameters tagged cycles_units=degrees are shown in degrees and authored in radians, also when
promoted to the material.

Houdini's own ramp convention (key count, first key basis, keys and values) cannot be used: it
loses per-key and monotone cubic interpolation, and its key count input has the name of the
Cycles socket.
"""

import math

import hou
from pxr import Gf, Sdf, Vt

from husd import shaderutils
from husd.shadertranslator import (RampParmTranslator, ShaderTranslator, ShaderTranslatorHelper,
                                   SimpleParmTranslator)

# RAMP_TABLE_SIZE in Cycles (kernel/types.h), the size Blender bakes color ramps and curves to.
TABLE_SIZE = 256
POSITIONS = [i / (TABLE_SIZE - 1) for i in range(TABLE_SIZE)]


def eval_ramp(parm_tuple, time_code):
    parm = parm_tuple[0]
    if time_code.IsDefault():
        return parm.evalAsRamp()
    return parm.evalAsRampAtFrame(time_code.GetValue())


class TableParmTranslator(RampParmTranslator):
    """Bakes the ramps of one Cycles table socket. Every ramp of the socket authors the whole
    table, so editing any channel updates it."""

    def attribName(self):
        return self.parmTuple().parmTemplate().tags()["cycles_socket"]

    def channels(self):
        socket = self.attribName()
        return {pt.parmTemplate().tags().get("cycles_channel"): pt
                for pt in self.parmTuple().node().parmTuples()
                if pt.parmTemplate().type() == hou.parmTemplateType.Ramp and
                pt.parmTemplate().tags().get("cycles_socket") == socket}

    def createAndSetAttrib(self, usd_shader, usd_time_code, metadata_infos=[]):
        time_code = self.adjustedTimeCode(usd_time_code)
        ramps = {c: eval_ramp(pt, time_code) for c, pt in self.channels().items()}
        if None in ramps:
            ramp = ramps[None]
            if ramp.isColor():
                sdf_type = Sdf.ValueTypeNames.Color3fArray
                value = Vt.Vec3fArray([Gf.Vec3f(*ramp.lookup(t)) for t in POSITIONS])
            else:
                sdf_type = Sdf.ValueTypeNames.FloatArray
                value = Vt.FloatArray([ramp.lookup(t) for t in POSITIONS])
        else:
            is_vector = "x" in ramps
            xyz = [ramps[c] for c in ("xyz" if is_vector else "rgb")]
            combined = ramps.get("c")
            values = []
            for t in POSITIONS:
                t = combined.lookup(t) if combined else t
                values.append(Gf.Vec3f(*(ramp.lookup(t) for ramp in xyz)))
            sdf_type = (Sdf.ValueTypeNames.Vector3fArray if is_vector else
                        Sdf.ValueTypeNames.Color3fArray)
            value = Vt.Vec3fArray(values)
        shaderutils.createUsdInputAttrib(usd_shader, self.attribName(), sdf_type).Set(
            value, time_code)

        # A Color Ramp with only constant keys is not interpolated, like in Blender.
        if self.attribName() == "ramp" and None in ramps and ramps[None].isColor():
            constant = all(b == hou.rampBasis.Constant for b in ramps[None].basis())
            shaderutils.createUsdInputAttrib(usd_shader, "interpolate",
                                             Sdf.ValueTypeNames.Bool).Set(not constant, time_code)


class RadiansParmTranslator(SimpleParmTranslator):
    """Angles shown in degrees, Cycles takes radians."""

    def valueFromParm(self, parm_tuple, usd_time_code):
        return tuple(math.radians(v) for v in
                     SimpleParmTranslator.valueFromParm(self, parm_tuple, usd_time_code))


class CyclesShaderTranslatorHelper(ShaderTranslatorHelper):
    def parameterTranslator(self, parm_tuple):
        tags = parm_tuple.parmTemplate().tags()
        if "cycles_socket" in tags:
            return TableParmTranslator(parm_tuple)
        if tags.get("cycles_units") == "degrees":
            return RadiansParmTranslator(parm_tuple)
        return ShaderTranslatorHelper.parameterTranslator(self, parm_tuple)

    def setUsdAncestorInputValue(self, parm_tuple, input_node, ancestor_input):
        # A promoted angle: the material parameter (or the Parameter VOP's default without one)
        # shows the degrees of the Cycles input it feeds, but has none of its tags.
        def feeds_degrees(connection):
            target = connection.outputNode().parmTuple(connection.inputName())
            return target is not None and \
                target.parmTemplate().tags().get("cycles_units") == "degrees"

        if not any(feeds_degrees(c) for c in input_node.outputConnections()):
            ShaderTranslatorHelper.setUsdAncestorInputValue(self, parm_tuple, input_node,
                                                            ancestor_input)
        elif parm_tuple:
            if shaderutils.isShaderParmActive(parm_tuple):
                RadiansParmTranslator(parm_tuple).setAttrib(ancestor_input.GetAttr(),
                                                            self.usdTimeCode())
        else:
            value_parm = shaderutils.valueParmTupleFromTypeParm(input_node, "parmtype")
            if value_parm:
                shaderutils.setUsdAttribFromParmValue(
                    ancestor_input, tuple(math.radians(v) for v in value_parm.eval()))


class CyclesShaderTranslator(ShaderTranslator):
    def matchesRenderMask(self, render_mask):
        return render_mask == "cycles"

    def shaderTranslatorHelper(self, translator_id, usd_stage, usd_material_path, usd_time_code):
        return CyclesShaderTranslatorHelper(translator_id, usd_stage, usd_material_path,
                                            usd_time_code)


cycles_translator = CyclesShaderTranslator()


def usdShaderTranslator():
    return cycles_translator
