/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/materialx.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "util/colorspace.h"

#include <memory>

#include <pxr/base/tf/getenv.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/dataSourceMaterialNetworkInterface.h>
#include <pxr/imaging/hd/materialConnectionSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hdMtlx/hdMtlx.h>
#include <pxr/usd/sdf/assetPath.h>

#include <MaterialXCore/Document.h>
#include <MaterialXFormat/XmlIo.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <set>
#include <tuple>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

namespace mx = MaterialX;

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(MaterialXTokens,
    (mtlx)
    (kma)
    (surface)
    (displacement)
    (volume)
);
// clang-format on

/* What a MaterialX value is in the Cycles graph. Vector2 values are 3D vectors with Z = 0, the
 * fourth component of color4 and vector4 values is a separate float. A matrix (Cycles has no
 * matrix type) is its rows, each a Vec3 (matrix33) or a Vec4 (matrix44). */
enum class Kind { None, Float, Vec3, Vec4, Closure, String, Matrix };

Kind KindOf(const std::string &type)
{
  if (type == "float" || type == "integer" || type == "boolean") {
    return Kind::Float;
  }
  if (type == "color3" || type == "vector3" || type == "vector2") {
    return Kind::Vec3;
  }
  if (type == "matrix33" || type == "matrix44") {
    return Kind::Matrix;
  }
  if (type == "color4" || type == "vector4") {
    return Kind::Vec4;
  }
  if (type == "string" || type == "filename") {
    return Kind::String;
  }
  if (type == "BSDF" || type == "EDF" || type == "VDF" || type == "surfaceshader" ||
      type == "displacementshader" || type == "volumeshader" || type == "material")
  {
    return Kind::Closure;
  }
  return Kind::None;
}

/* A translated MaterialX value: a Cycles node output or a constant. */
struct Val {
  Kind kind = Kind::None;
  /* Float, Vec3 or Closure output, or the color of a Vec4. */
  ShaderOutput *out = nullptr;
  /* Fourth component of a Vec4. */
  ShaderOutput *alpha = nullptr;
  /* Constant, for the parts without an output. */
  float4 c = zero_float4();
  std::string str;
  /* Color space of a file name. */
  std::string colorspace;
  /* Value of a default geometric property (shading normal, tangent, ...), that Cycles nodes
   * already use when their input is not connected. */
  bool geomDefault = false;
  /* Output of chiang_hair_roughness: the Cycles hair roughnesses (longitudinal, azimuthal), from
   * which Cycles computes the variance and logistic scale itself, instead of those. */
  bool hairRoughness = false;
  /* Output of heighttonormal: Cycles' Bump node, whose Normal is already a world space normal (MaterialX's is a
   * tangent space normal encoded as a color). The normalmap it feeds (MaterialX's bump) completes the Bump node
   * instead of decoding that normal again: bumpNode's Distance is linked there, from bumpScale. */
  ShaderNode *bumpNode = nullptr;
  std::shared_ptr<Val> bumpScale;
  /* Rows of a matrix, 3 or 4; none for a zero matrix. */
  std::shared_ptr<std::vector<Val>> rows;
  /* Throughput of a BSDF, as MaterialX's GLSL layers them: the share of light that passes it to a base below
   * (1 - its directional albedo * weight), a float. None: 1. */
  std::shared_ptr<Val> throughput;
  /* The closure without its transmission, where it differs (dielectrics that transmit): what a layer over an
   * opaque base shows of it, the transmitted light going into the base. */
  std::shared_ptr<Val> opaque;

  bool isConstant() const
  {
    return out == nullptr && alpha == nullptr;
  }
};

Val Const(const float x)
{
  Val v;
  v.kind = Kind::Float;
  v.c = make_float4(x, x, x, x);
  return v;
}

Val Const(const float x, const float y, const float z)
{
  Val v;
  v.kind = Kind::Vec3;
  v.c = make_float4(x, y, z, 1.0f);
  return v;
}

Val Of(ShaderOutput *out, const Kind kind)
{
  Val v;
  v.kind = kind;
  v.out = out;
  return v;
}

float Get(const float4 &c, const int i)
{
  return i == 0 ? c.x : i == 1 ? c.y : i == 2 ? c.z : c.w;
}

/* Cycles hair roughness for a chiang_hair_bsdf longitudinal variance or azimuthal logistic scale:
 * inverts the polynomials of chiang_hair_roughness, which Cycles applies itself. */
float HairRoughness(float value, const bool azimuthal)
{
  value = std::clamp(value, 0.001f, 1.0f);
  float lo = 0.0f, hi = 1.0f;
  for (int i = 0; i < 32; i++) {
    const float b = 0.5f * (lo + hi);
    const float f = azimuthal ? 0.265f * b + 1.194f * b * b + 5.372f * powf(b, 22.0f) :
                                sqr(0.726f * b + 0.812f * b * b + 3.7f * powf(b, 20.0f));
    (f < value ? lo : hi) = b;
  }
  return 0.5f * (lo + hi);
}

std::string NormalizeName(const std::string &name)
{
  std::string result;
  for (const char c : name) {
    if (c != '_' && c != ' ') {
      result += std::tolower(static_cast<unsigned char>(c));
    }
  }
  return result;
}

/* Node definition of a node. Inputs of other types than the definition (as Houdini authors when
 * e.g. a vector is connected to a color) need a rough match. */
mx::NodeDefPtr NodeDef(const mx::NodePtr &node)
{
  mx::NodeDefPtr nodeDef = node->getNodeDef();
  return nodeDef ? nodeDef : node->getNodeDef(mx::EMPTY_STRING, true);
}

bool IsSpace(const std::string &space, const char *name)
{
  return space == name || (std::string(name) == "object" && space == "model");
}

class Translator {
 public:
  Translator(ShaderGraph *graph, mx::DocumentPtr doc, const SdfPath &materialId)
      : _graph(graph), _doc(std::move(doc)), _materialId(materialId)
  {
  }

  /* Value of an output of a node in the document. */
  Val Eval(const mx::NodePtr &node, const std::string &output)
  {
    return Eval(node, output, nullptr);
  }

  /* Connect an output of a node in the document to an input, or set it as constant. */
  bool LinkOutput(const mx::NodePtr &node, const std::string &output, ShaderInput *in)
  {
    const Val v = Eval(node, output, nullptr);
    Link(v, in);
    return v.kind != Kind::None;
  }

 private:
  /* Expansion of a node defined by a node graph: interface inputs of the graph resolve to the
   * inputs of the node, in the scope of the node. */
  struct Scope {
    mx::NodePtr instance;
    const Scope *parent;
  };

  ShaderGraph *_graph;
  mx::DocumentPtr _doc;
  SdfPath _materialId;
  std::deque<Scope> _scopes;
  std::map<std::tuple<const Scope *, const mx::Element *, std::string>, Val> _evaluated;
  std::map<ShaderOutput *, ShaderNode *> _separated;
  std::set<std::string> _warned;

  void Warn(const std::string &message)
  {
    if (_warned.insert(message).second) {
      TF_WARN("MaterialX material %s: %s", _materialId.GetText(), message.c_str());
    }
  }

  /* Cycles graph helpers. */

  ShaderNode *Create(const char *type)
  {
    const NodeType *nodeType = NodeType::find(ustring(type));
    assert(nodeType);
    return _graph->create_node(nodeType);
  }

  static ShaderInput *In(ShaderNode *node, const char *name)
  {
    const std::string normName = NormalizeName(name);
    for (ShaderInput *in : node->inputs) {
      if (NormalizeName(in->name().string()) == normName) {
        return in;
      }
    }
    for (ShaderInput *in : node->inputs) {
      if (NormalizeName(in->socket_type.name.string()) == normName) {
        return in;
      }
    }
    TF_CODING_ERROR("No input %s on Cycles node %s", name, node->type->name.c_str());
    return nullptr;
  }

  static ShaderOutput *Out(ShaderNode *node, const char *name)
  {
    const std::string normName = NormalizeName(name);
    for (ShaderOutput *out : node->outputs) {
      if (NormalizeName(out->name().string()) == normName ||
          NormalizeName(out->socket_type.name.string()) == normName)
      {
        return out;
      }
    }
    TF_CODING_ERROR("No output %s on Cycles node %s", name, node->type->name.c_str());
    return nullptr;
  }

  static void SetEnum(ShaderNode *node, const char *socketName, const char *value)
  {
    const SocketType *socket = node->type->find_input(ustring(socketName));
    if (socket && socket->enum_values && socket->enum_values->exists(ustring(value))) {
      node->set(*socket, ustring(value));
    }
    else {
      TF_CODING_ERROR("No value %s for %s on %s", value, socketName, node->type->name.c_str());
    }
  }

  static void SetString(ShaderNode *node, const char *socketName, const std::string &value)
  {
    if (const SocketType *socket = node->type->find_input(ustring(socketName))) {
      node->set(*socket, ustring(value));
    }
  }

  static void SetBool(ShaderNode *node, const char *socketName, const bool value)
  {
    if (const SocketType *socket = node->type->find_input(ustring(socketName))) {
      node->set(*socket, value);
    }
  }

  /* Connect a value to an input, or set it as constant. */
  void Link(const Val &v, ShaderInput *in)
  {
    if (!in || v.kind == Kind::None) {
      return;
    }
    if (v.out) {
      _graph->connect(v.out, in);
      return;
    }
    Node *node = in->parent;
    const SocketType &socket = in->socket_type;
    switch (socket.type) {
      case SocketType::FLOAT:
        node->set(socket, v.c.x);
        break;
      case SocketType::INT:
        node->set(socket, int(v.c.x));
        break;
      case SocketType::BOOLEAN:
        node->set(socket, v.c.x != 0.0f);
        break;
      case SocketType::COLOR:
      case SocketType::VECTOR:
      case SocketType::POINT:
      case SocketType::NORMAL:
        node->set(socket, make_float3(v.c.x, v.c.y, v.c.z));
        break;
      case SocketType::STRING:
        node->set(socket, ustring(v.str));
        break;
      default:
        break;
    }
  }

  /* Link unless the value is the default geometric property, that Cycles uses already. */
  void LinkGeom(const Val &v, ShaderInput *in)
  {
    if (!v.geomDefault) {
      Link(v, in);
    }
  }

  Val Component(const Val &v, const int i)
  {
    if (v.kind == Kind::Float || v.kind == Kind::None) {
      return v;
    }
    if (i == 3) {
      if (v.kind == Kind::Vec4 && v.alpha) {
        return Of(v.alpha, Kind::Float);
      }
      return Const(v.kind == Kind::Vec4 ? v.c.w : 1.0f);
    }
    if (!v.out) {
      return Const(Get(v.c, i));
    }
    ShaderNode *&separate = _separated[v.out];
    if (!separate) {
      separate = Create("separate_xyz");
      _graph->connect(v.out, In(separate, "Vector"));
    }
    return Of(Out(separate, i == 0 ? "X" : i == 1 ? "Y" : "Z"), Kind::Float);
  }

  Val Combine(const Val &x, const Val &y, const Val &z)
  {
    if (x.isConstant() && y.isConstant() && z.isConstant()) {
      return Const(x.c.x, y.c.x, z.c.x);
    }
    ShaderNode *node = Create("combine_xyz");
    Link(Cast(x, Kind::Float), In(node, "X"));
    Link(Cast(y, Kind::Float), In(node, "Y"));
    Link(Cast(z, Kind::Float), In(node, "Z"));
    return Of(Out(node, "Vector"), Kind::Vec3);
  }

  static Val WithAlpha(Val rgb, const Val &alpha)
  {
    rgb.kind = Kind::Vec4;
    rgb.alpha = alpha.out;
    rgb.c.w = alpha.c.x;
    return rgb;
  }

  Val Cast(Val v, const Kind kind)
  {
    if (kind == Kind::None || v.kind == kind || v.kind == Kind::None || v.kind == Kind::Closure ||
        v.kind == Kind::String)
    {
      return v;
    }
    switch (kind) {
      case Kind::Float:
        return Component(v, 0);
      case Kind::Vec3:
        if (v.kind == Kind::Float) {
          /* Cycles broadcasts a float output linked to a vector input. */
          v.c = make_float4(v.c.x, v.c.x, v.c.x, 1.0f);
        }
        v.kind = Kind::Vec3;
        v.alpha = nullptr;
        return v;
      case Kind::Vec4:
        if (v.kind == Kind::Float) {
          v.alpha = v.out;
          v.c = make_float4(v.c.x, v.c.x, v.c.x, v.c.x);
        }
        else {
          v.c.w = 1.0f;
          v.alpha = nullptr;
        }
        v.kind = Kind::Vec4;
        return v;
      default:
        return v;
    }
  }

  Val Math(const char *op, const Val &a, const Val &b = Const(0.0f), const Val &c = Const(0.0f))
  {
    ShaderNode *node = Create("math");
    SetEnum(node, "math_type", op);
    Link(Cast(a, Kind::Float), In(node, "Value1"));
    Link(Cast(b, Kind::Float), In(node, "Value2"));
    Link(Cast(c, Kind::Float), In(node, "Value3"));
    return Of(Out(node, "Value"), Kind::Float);
  }

  Val VMath(const char *op,
            const Val &a,
            const Val &b = Const(0.0f),
            const Val &c = Const(0.0f),
            const bool floatResult = false)
  {
    ShaderNode *node = Create("vector_math");
    SetEnum(node, "math_type", op);
    Link(Cast(a, Kind::Vec3), In(node, "Vector1"));
    Link(Cast(b, Kind::Vec3), In(node, "Vector2"));
    Link(Cast(c, Kind::Vec3), In(node, "Vector3"));
    return floatResult ? Of(Out(node, "Value"), Kind::Float) : Of(Out(node, "Vector"), Kind::Vec3);
  }

  Val Scale(const Val &v, const Val &scale)
  {
    ShaderNode *node = Create("vector_math");
    SetEnum(node, "math_type", "scale");
    Link(Cast(v, Kind::Vec3), In(node, "Vector1"));
    Link(Cast(scale, Kind::Float), In(node, "Scale"));
    return Of(Out(node, "Vector"), Kind::Vec3);
  }

  /* Component wise operation, with the vector math operation for 3D values if there is one. */
  Val Op(const Kind kind,
         const char *mathOp,
         const char *vectorOp,
         const Val &a,
         const Val &b = Const(0.0f),
         const Val &c = Const(0.0f))
  {
    switch (kind) {
      case Kind::Float:
        return Math(mathOp, a, b, c);
      case Kind::Vec3: {
        if (vectorOp) {
          return VMath(vectorOp, a, b, c);
        }
        const Val a3 = Cast(a, Kind::Vec3), b3 = Cast(b, Kind::Vec3), c3 = Cast(c, Kind::Vec3);
        Val result[3];
        for (int i = 0; i < 3; i++) {
          result[i] = Math(mathOp, Component(a3, i), Component(b3, i), Component(c3, i));
        }
        return Combine(result[0], result[1], result[2]);
      }
      case Kind::Vec4: {
        const Val a4 = Cast(a, Kind::Vec4), b4 = Cast(b, Kind::Vec4), c4 = Cast(c, Kind::Vec4);
        const Val rgb = Op(Kind::Vec3, mathOp, vectorOp, a4, b4, c4);
        return WithAlpha(rgb, Math(mathOp, Component(a4, 3), Component(b4, 3), Component(c4, 3)));
      }
      default:
        return Val();
    }
  }

  /* Dot product of two values of the same kind, the fourth components included. */
  Val Dot(const Val &a, const Val &b)
  {
    const Val xyz = VMath("dot_product", a, b, Const(0.0f), true);
    if (a.kind != Kind::Vec4 && b.kind != Kind::Vec4) {
      return xyz;
    }
    return Math("multiply_add", Component(a, 3), Component(b, 3), xyz);
  }

  /* Matrices. MaterialX's (as its OSL and GLSL code) multiply row vectors: out_j = sum_i in_i m_ij,
   * the last row is the translation, A * B applies A first. Constant matrices are computed here,
   * others with Cycles' vector math, row by row. */
  struct MatC {
    int n;
    float m[4][4];
  };

  static Val MatrixOf(std::vector<Val> rows)
  {
    Val v;
    v.kind = Kind::Matrix;
    v.rows = std::make_shared<std::vector<Val>>(std::move(rows));
    return v;
  }

  static int Size(const Val &m)
  {
    return m.rows ? int(m.rows->size()) : 0;
  }

  static Kind RowKind(const int n)
  {
    return n == 4 ? Kind::Vec4 : Kind::Vec3;
  }

  static Val ConstRow(const float4 &c, const int n)
  {
    Val v;
    v.kind = RowKind(n);
    v.c = n == 4 ? c : make_float4(c.x, c.y, c.z, 1.0f);
    return v;
  }

  /* Row i of an n x n matrix: a missing one (a zero matrix) is zero. */
  Val Row(const Val &m, const int i, const int n)
  {
    if (i < Size(m)) {
      return Cast((*m.rows)[i], RowKind(n));
    }
    return ConstRow(zero_float4(), n);
  }

  static Val FromMatC(const MatC &c)
  {
    std::vector<Val> rows;
    for (int i = 0; i < c.n; i++) {
      rows.push_back(ConstRow(make_float4(c.m[i][0], c.m[i][1], c.m[i][2], c.m[i][3]), c.n));
    }
    return MatrixOf(std::move(rows));
  }

  bool ToMatC(const Val &m, const int n, MatC &c)
  {
    c.n = n;
    for (int i = 0; i < n; i++) {
      const Val row = Row(m, i, n);
      if (!row.isConstant()) {
        return false;
      }
      for (int j = 0; j < 4; j++) {
        c.m[i][j] = j < n ? Get(row.c, j) : 0.0f;
      }
    }
    return true;
  }

  static MatC MulC(const MatC &a, const MatC &b)
  {
    MatC c{a.n, {}};
    for (int i = 0; i < a.n; i++) {
      for (int j = 0; j < a.n; j++) {
        for (int k = 0; k < a.n; k++) {
          c.m[i][j] += a.m[i][k] * b.m[k][j];
        }
      }
    }
    return c;
  }

  /* The matrix without one row and one column. */
  static MatC MinorC(const MatC &a, const int row, const int column)
  {
    MatC minor{a.n - 1, {}};
    for (int i = 0, mi = 0; i < a.n; i++) {
      if (i == row) {
        continue;
      }
      for (int j = 0, mj = 0; j < a.n; j++) {
        if (j != column) {
          minor.m[mi][mj++] = a.m[i][j];
        }
      }
      mi++;
    }
    return minor;
  }

  /* Laplace along the first row: enough for 3 x 3 and 4 x 4. */
  static float DetC(const MatC &a)
  {
    if (a.n == 2) {
      return a.m[0][0] * a.m[1][1] - a.m[0][1] * a.m[1][0];
    }
    float det = 0.0f;
    for (int j = 0; j < a.n; j++) {
      det += (j % 2 ? -1.0f : 1.0f) * a.m[0][j] * DetC(MinorC(a, 0, j));
    }
    return det;
  }

  /* Inverse by cofactors, a zero matrix for a singular one (as OSL's 1 / m). */
  static MatC InverseC(const MatC &a)
  {
    MatC inverse{a.n, {}};
    const float det = DetC(a);
    if (fabsf(det) < 1e-30f) {
      return inverse;
    }
    for (int i = 0; i < a.n; i++) {
      for (int j = 0; j < a.n; j++) {
        inverse.m[j][i] = ((i + j) % 2 ? -1.0f : 1.0f) * DetC(MinorC(a, i, j)) / det;
      }
    }
    return inverse;
  }

  /* x * row: a row (Vec3 / Vec4) scaled by a float. */
  Val ScaleRow(const Val &row, const Val &x, const int n)
  {
    return Op(RowKind(n), "multiply", "multiply", row, Cast(x, RowKind(n)));
  }

  /* sum_i v_i row_i over the first `count` rows, plus row `count` when `point`. */
  Val CombineRows(const Val &m, const int n, const Val &v, const int count, const bool point)
  {
    Val sum = ScaleRow(Row(m, 0, n), Component(v, 0), n);
    for (int i = 1; i < count; i++) {
      sum = Op(RowKind(n), "add", "add", sum, ScaleRow(Row(m, i, n), Component(v, i), n));
    }
    return point ? Op(RowKind(n), "add", "add", sum, Row(m, count, n)) : sum;
  }

  /* 3 x 3 determinant of three Vec3 rows: a . (b x c). */
  Val Det3(const Val &a, const Val &b, const Val &c)
  {
    return VMath("dot_product", a, VMath("cross_product", b, c), Const(0.0f), true);
  }

  template<typename InputFn>
  Val MatrixNode(const std::string &category,
                 const mx::NodePtr &node,
                 const InputFn &input,
                 bool &supported)
  {
    const mx::NodeDefPtr nodeDef = NodeDef(node);
    const auto size = [&](const char *name) {
      const mx::InputPtr def = nodeDef->getActiveInput(name);
      return def && def->getType() == "matrix44" ? 4 : 3;
    };
    const int n = nodeDef->getType() == "matrix44" ? 4 : size(category == "creatematrix" ? "in4" : "in");
    MatC a, b;
    if (category == "add" || category == "subtract") {
      const Val m1 = input("in1"), m2 = input("in2");
      const bool scalar = m2.kind != Kind::Matrix;
      if (ToMatC(m1, n, a) && (scalar ? m2.isConstant() : ToMatC(m2, n, b))) {
        for (int i = 0; i < n; i++) {
          for (int j = 0; j < n; j++) {
            const float y = scalar ? m2.c.x : b.m[i][j];
            a.m[i][j] += category == "add" ? y : -y;
          }
        }
        return FromMatC(a);
      }
      std::vector<Val> rows;
      for (int i = 0; i < n; i++) {
        rows.push_back(Op(RowKind(n),
                          category.c_str(),
                          category.c_str(),
                          Row(m1, i, n),
                          scalar ? Cast(m2, RowKind(n)) : Row(m2, i, n)));
      }
      return MatrixOf(std::move(rows));
    }
    if (category == "multiply" || category == "divide") {
      const Val m1 = input("in1"), m2 = input("in2");
      const bool divide = category == "divide";
      if (!ToMatC(m2, n, b)) {
        if (divide) {
          supported = false; /* The inverse of a varying matrix. */
          return Val();
        }
      }
      else if (divide) {
        b = InverseC(b);
      }
      if (ToMatC(m1, n, a) && (divide || ToMatC(m2, n, b))) {
        return FromMatC(MulC(a, b));
      }
      const Val right = divide ? FromMatC(b) : m2;
      std::vector<Val> rows;
      for (int i = 0; i < n; i++) {
        rows.push_back(CombineRows(right, n, Row(m1, i, n), n, false));
      }
      return MatrixOf(std::move(rows));
    }
    if (category == "transpose") {
      const Val m = input("in");
      if (ToMatC(m, n, a)) {
        MatC t{n, {}};
        for (int i = 0; i < n; i++) {
          for (int j = 0; j < n; j++) {
            t.m[i][j] = a.m[j][i];
          }
        }
        return FromMatC(t);
      }
      std::vector<Val> rows;
      for (int j = 0; j < n; j++) {
        const Val xyz = Combine(Component(Row(m, 0, n), j),
                                Component(Row(m, 1, n), j),
                                Component(Row(m, 2, n), j));
        rows.push_back(n == 4 ? WithAlpha(xyz, Component(Row(m, 3, n), j)) : xyz);
      }
      return MatrixOf(std::move(rows));
    }
    if (category == "determinant") {
      const Val m = input("in");
      if (ToMatC(m, n, a)) {
        return Const(DetC(a));
      }
      if (n == 3) {
        return Det3(Row(m, 0, 3), Row(m, 1, 3), Row(m, 2, 3));
      }
      /* Along the first row: the 3 x 3 minors of rows 1-3 without column j. */
      Val det = Const(0.0f);
      for (int j = 0; j < 4; j++) {
        Val minor[3];
        for (int i = 0; i < 3; i++) {
          const Val row = Row(m, i + 1, 4);
          Val c[3];
          for (int k = 0, l = 0; k < 4; k++) {
            if (k != j) {
              c[l++] = Component(row, k);
            }
          }
          minor[i] = Combine(c[0], c[1], c[2]);
        }
        const Val term = Math("multiply", Component(Row(m, 0, 4), j), Det3(minor[0], minor[1], minor[2]));
        det = Math(j % 2 ? "subtract" : "add", det, term);
      }
      return det;
    }
    if (category == "invertmatrix") {
      if (ToMatC(input("in"), n, a)) {
        return FromMatC(InverseC(a));
      }
      supported = false; /* The inverse of a varying matrix. */
      return Val();
    }
    if (category == "creatematrix") {
      const bool vector4 = nodeDef->getActiveInput("in1")->getType() == "vector4";
      std::vector<Val> rows;
      const char *names[] = {"in1", "in2", "in3", "in4"};
      for (int i = 0; i < n; i++) {
        const Val in = input(names[i]);
        if (n == 3 || vector4) {
          rows.push_back(Cast(in, RowKind(n)));
        }
        else {
          rows.push_back(WithAlpha(Cast(in, Kind::Vec3), Const(i == 3 ? 1.0f : 0.0f)));
        }
      }
      return MatrixOf(std::move(rows));
    }
    if (category == "transformmatrix") {
      const Val m = input("mat");
      const std::string inType = nodeDef->getActiveInput("in")->getType();
      const int msize = size("mat");
      const Val in = input("in");
      if (inType == "vector2") {
        /* (x, y, 1): the third row translates. */
        return CombineRows(m, 3, in, 2, true);
      }
      if (inType == "vector3" && msize == 4) {
        /* A point (x, y, z, 1), as MaterialX's GLSL (its OSL leaves the translation out). */
        return Cast(CombineRows(m, 4, in, 3, true), Kind::Vec3);
      }
      return CombineRows(m, msize, in, msize, false);
    }
    supported = false;
    return Val();
  }

  Val Mul(const Kind kind, const Val &a, const Val &b)
  {
    return Op(kind, "multiply", "multiply", a, b);
  }

  /* Mix from bg (factor 0) to fg (factor 1). */
  Val Mix(const Kind kind, const Val &fac, const Val &bg, const Val &fg)
  {
    switch (kind) {
      case Kind::Float: {
        ShaderNode *node = Create("mix_float");
        Link(Cast(fac, Kind::Float), In(node, "Factor"));
        Link(Cast(bg, Kind::Float), In(node, "A"));
        Link(Cast(fg, Kind::Float), In(node, "B"));
        return Of(Out(node, "Result"), Kind::Float);
      }
      case Kind::Vec3: {
        const bool uniform = fac.kind == Kind::Float;
        ShaderNode *node = Create(uniform ? "mix_vector" : "mix_vector_non_uniform");
        Link(uniform ? fac : Cast(fac, Kind::Vec3), In(node, "Factor"));
        Link(Cast(bg, Kind::Vec3), In(node, "A"));
        Link(Cast(fg, Kind::Vec3), In(node, "B"));
        return Of(Out(node, "Result"), Kind::Vec3);
      }
      case Kind::Vec4: {
        const Val bg4 = Cast(bg, Kind::Vec4), fg4 = Cast(fg, Kind::Vec4);
        const Val rgb = Mix(
            Kind::Vec3, fac.kind == Kind::Vec4 ? Cast(fac, Kind::Vec3) : fac, bg4, fg4);
        return WithAlpha(
            rgb, Mix(Kind::Float, Component(fac, 3), Component(bg4, 3), Component(fg4, 3)));
      }
      case Kind::Matrix: {
        const int n = std::max(Size(bg), Size(fg));
        std::vector<Val> rows;
        for (int i = 0; i < n; i++) {
          rows.push_back(Mix(RowKind(n), fac, Row(bg, i, n), Row(fg, i, n)));
        }
        return MatrixOf(std::move(rows));
      }
      case Kind::Closure: {
        ShaderNode *node = Create("mix_closure");
        Link(Cast(fac, Kind::Float), In(node, "Fac"));
        Link(bg, In(node, "Closure1"));
        Link(fg, In(node, "Closure2"));
        return Of(Out(node, "Closure"), Kind::Closure);
      }
      default:
        return Val();
    }
  }

  Val AddClosures(const Val &a, const Val &b)
  {
    if (!a.out) {
      return b;
    }
    if (!b.out) {
      return a;
    }
    ShaderNode *node = Create("add_closure");
    Link(a, In(node, "Closure1"));
    Link(b, In(node, "Closure2"));
    return Of(Out(node, "Closure"), Kind::Closure);
  }

  /* Closure scaled by a weight, as Cycles closures only have weights through their colors. */
  Val Weighted(const Val &closure, const Val &weight)
  {
    if (weight.isConstant() && weight.c.x == 1.0f) {
      return closure;
    }
    ShaderNode *node = Create("mix_closure");
    Link(Cast(weight, Kind::Float), In(node, "Fac"));
    Link(closure, In(node, "Closure2"));
    return Of(Out(node, "Closure"), Kind::Closure);
  }

  static bool IsEmission(const Val &closure)
  {
    return closure.out && closure.out->parent->type->name == "emission";
  }

  /* An EDF scaled by a colour: an Emission node's colour times it (another closure by its average). */
  Val ScaleEmission(const Val &edf, const Val &color)
  {
    if (!IsEmission(edf)) {
      return Weighted(edf, Average(color));
    }
    ShaderNode *old = edf.out->parent;
    const auto inputOf = [&](const char *name, const Kind kind) {
      ShaderInput *in = In(old, name);
      if (in->link) {
        return Of(in->link, kind);
      }
      return kind == Kind::Float ? Const(old->get_float(in->socket_type)) :
                                   Const(old->get_float3(in->socket_type).x,
                                         old->get_float3(in->socket_type).y,
                                         old->get_float3(in->socket_type).z);
    };
    ShaderNode *emission = Create("emission");
    Link(Op(Kind::Vec3, "multiply", "multiply", inputOf("Color", Kind::Vec3), Cast(color, Kind::Vec3)),
         In(emission, "Color"));
    Link(inputOf("Strength", Kind::Float), In(emission, "Strength"));
    return Of(Out(emission, "Emission"), Kind::Closure);
  }

  /* "Artist Friendly Metallic Fresnel" (Gulbrandsen 2014), as pbrlib's mx_artistic_ior: the complex IOR of a
   * metal of a reflectivity and an edge colour. */
  Val ArtisticIor(const Val &reflectivity, const Val &edgeColor, const bool extinction)
  {
    const auto channel = [](float r, const float edge, float &n, float &k) {
      r = std::clamp(r, 0.0f, 0.99f);
      const float rs = sqrtf(r);
      const float nMin = (1.0f - r) / (1.0f + r), nMax = (1.0f + rs) / (1.0f - rs);
      n = nMax + (nMin - nMax) * edge;
      k = sqrtf(std::max(((n + 1.0f) * (n + 1.0f) * r - (n - 1.0f) * (n - 1.0f)) / (1.0f - r), 0.0f));
    };
    if (reflectivity.isConstant() && edgeColor.isConstant()) {
      float n[3], k[3];
      for (int i = 0; i < 3; i++) {
        channel(Get(reflectivity.c, i), Get(edgeColor.c, i), n[i], k[i]);
      }
      return extinction ? Const(k[0], k[1], k[2]) : Const(n[0], n[1], n[2]);
    }
    Val result[3];
    for (int i = 0; i < 3; i++) {
      const Val r = Math("minimum", Math("maximum", Component(reflectivity, i), Const(0.0f)), Const(0.99f));
      const Val rs = Math("sqrt", r);
      const Val nMin = Math("divide", Math("subtract", Const(1.0f), r), Math("add", Const(1.0f), r));
      const Val nMax = Math("divide", Math("add", Const(1.0f), rs), Math("subtract", Const(1.0f), rs));
      const Val n = Mix(Kind::Float, Component(edgeColor, i), nMax, nMin);
      if (!extinction) {
        result[i] = n;
        continue;
      }
      const Val np1 = Math("add", n, Const(1.0f)), nm1 = Math("subtract", n, Const(1.0f));
      const Val k2 = Math("divide",
                          Math("subtract", Math("multiply", Math("multiply", np1, np1), r), Math("multiply", nm1, nm1)),
                          Math("subtract", Const(1.0f), r));
      result[i] = Math("sqrt", Math("maximum", k2, Const(0.0f)));
    }
    return Combine(result[0], result[1], result[2]);
  }

  static Val WithThroughput(Val closure, const Val &throughput)
  {
    closure.throughput = std::make_shared<Val>(throughput);
    return closure;
  }

  static Val OpaqueOf(const Val &closure)
  {
    return closure.opaque ? *closure.opaque : closure;
  }

  static Val ThroughputOf(const Val &closure)
  {
    return closure.throughput ? *closure.throughput : Const(1.0f);
  }

  /* |N.V| of a BSDF's normal, at least MaterialX's M_FLOAT_EPS (Geometry's Incoming points to the viewer). */
  Val NdotV(const Val &normal)
  {
    const Val n = normal.kind == Kind::None ? Geometry("Normal") : normal;
    return Math("maximum",
                Math("absolute", VMath("dot_product", n, Geometry("Incoming"), Const(0.0f), true)),
                Const(1e-8f));
  }

  /* MaterialX's GGX directional albedo fit (its GLSL's mx_ggx_dir_albedo_analytic): A and B of
   * E = F0 A + F90 B, a rational function of x = N.V and y = the average alpha. With a constant alpha it is a
   * quadratic in x per term (Cycles folds the constant math). */
  std::pair<Val, Val> GgxDirAlbedo(const Val &x, const Val &y)
  {
    static const float c[9][4] = {{0.1003f, 0.9345f, 1.0f, 1.0f},
                                  {-0.6303f, -2.323f, -1.765f, 0.2281f},
                                  {9.748f, 2.229f, 8.263f, 15.94f},
                                  {-2.038f, -3.748f, 11.53f, -55.83f},
                                  {29.34f, 1.424f, 28.96f, 13.08f},
                                  {-8.245f, -0.7684f, -7.507f, 41.26f},
                                  {-26.44f, 1.436f, -36.11f, 54.9f},
                                  {19.99f, 0.2913f, 15.86f, 300.2f},
                                  {-5.448f, 0.6286f, 33.37f, -285.1f}};
    Val r[4];
    for (int k = 0; k < 4; k++) {
      /* r = (c0 + c2 y + c5 y^2) + x (c1 + c3 y + c7 y^2) + x^2 (c4 + c6 y + c8 y^2) */
      const auto quad = [&](const int a, const int b, const int d) {
        if (y.isConstant()) {
          return Const(c[a][k] + c[b][k] * y.c.x + c[d][k] * y.c.x * y.c.x);
        }
        return Math("multiply_add", Math("multiply_add", Const(c[d][k]), y, Const(c[b][k])), y, Const(c[a][k]));
      };
      const Val a0 = quad(0, 2, 5), a1 = quad(1, 3, 7), a2 = quad(4, 6, 8);
      r[k] = Math("multiply_add", Math("multiply_add", a2, x, a1), x, a0);
    }
    const auto clamp01 = [&](const Val &v) {
      return Math("minimum", Math("maximum", v, Const(0.0f)), Const(1.0f));
    };
    return {clamp01(Math("divide", r[0], r[2])), clamp01(Math("divide", r[1], r[3]))};
  }

  Val AverageAlpha(const Val &alpha)
  {
    return Math("sqrt", Math("multiply", Component(alpha, 0), Component(alpha, 1)));
  }

  /* MaterialX's dielectric_bsdf: 1 - weight * (F0 A + B) * (1 + F (1 - Ess) / Ess), F the Fresnel at N.V,
   * Ess = A + B (its energy compensation). */
  template<typename InputFn> Val DielectricThroughput(const InputFn &input)
  {
    const Val ior = input("ior");
    const Val x = NdotV(input("normal"));
    const auto [a, b] = GgxDirAlbedo(x, AverageAlpha(input("roughness")));
    const Val f0r = Math("divide", Math("subtract", ior, Const(1.0f)), Math("add", ior, Const(1.0f)));
    const Val f0 = Math("multiply", f0r, f0r);
    ShaderNode *fresnel = Create("fresnel");
    Link(ior, In(fresnel, "IOR"));
    LinkGeom(input("normal"), In(fresnel, "Normal"));
    const Val f = Of(Out(fresnel, "Fac"), Kind::Float);
    const Val ess = Math("maximum", Math("add", a, b), Const(1e-8f));
    const Val comp = Math("multiply_add", f, Math("divide", Math("subtract", Const(1.0f), ess), ess), Const(1.0f));
    const Val albedo = Math("multiply", Math("multiply_add", f0, a, b), comp);
    return Math("subtract", Const(1.0f), Math("multiply", albedo, input("weight")));
  }

  /* MaterialX's generalized_schlick_bsdf: 1 - weight * average((color0 A + color90 B) comp), F the generalized
   * Schlick Fresnel at N.V. */
  template<typename InputFn> Val SchlickThroughput(const InputFn &input)
  {
    const Val x = NdotV(input("normal"));
    const auto [a, b] = GgxDirAlbedo(x, AverageAlpha(input("roughness")));
    const Val c0 = input("color0"), c90 = input("color90");
    const Val s = Math("power", Math("subtract", Const(1.0f), x), input("exponent"));
    const Val f = Mix(Kind::Vec3, s, c0, c90);
    const Val ess = Math("maximum", Math("add", a, b), Const(1e-8f));
    const Val k = Math("divide", Math("subtract", Const(1.0f), ess), ess);
    const Val comp = Op(Kind::Vec3, "multiply_add", "multiply_add", f, Cast(k, Kind::Vec3), Const(1.0f, 1.0f, 1.0f));
    const Val albedo = Op(Kind::Vec3,
                          "multiply",
                          "multiply",
                          Op(Kind::Vec3, "add", "add", Mul(Kind::Vec3, c0, Cast(a, Kind::Vec3)), Mul(Kind::Vec3, c90, Cast(b, Kind::Vec3))),
                          comp);
    return Math("subtract", Const(1.0f), Math("multiply", Average(albedo), input("weight")));
  }

  /* MaterialX's sheen_bsdf: 1 - weight * its directional albedo (Conty-Kulla's rational fit or Zeltner's). */
  template<typename InputFn> Val SheenThroughput(const InputFn &input)
  {
    const Val x = NdotV(input("normal"));
    Val albedo;
    if (input("mode").str == "zeltner") {
      const Val y = Math("minimum", Math("maximum", input("roughness"), Const(0.01f)), Const(1.0f));
      const auto poly = [&](std::initializer_list<float> coefficients) {
        /* Horner in y, highest power first. */
        Val v = Const(0.0f);
        for (const float coefficient : coefficients) {
          v = Math("multiply_add", v, y, Const(coefficient));
        }
        return v;
      };
      /* s, m, o of the Gaussian fit (mx_zeltner_sheen_dir_albedo). */
      const Val s = Math("divide",
                         Math("multiply", y, poly({1.58491f, 0.0206607f})),
                         poly({1.0f, 1.32227f, 0.0379424f}));
      const Val m = Math("divide",
                         Math("multiply", y, poly({-0.95943f, 0.0f, 1.7932f, -1.14885f, -0.193854f})),
                         Math("add", y, Const(0.046391f)));
      const Val o = Math("divide",
                         Math("multiply", y, poly({0.119681f, -0.0207818f, 0.000654023f})),
                         poly({1.0f, -1.92021f, 1.26264f}));
      const Val z = Math("divide", Math("subtract", x, m), s);
      const Val gauss = Math("divide",
                             Math("exponent", Math("multiply", Math("multiply", z, z), Const(-0.5f))),
                             Math("multiply", s, Const(sqrtf(2.0f * M_PI_F))));
      albedo = Math("add", gauss, o);
    }
    else {
      const Val y = input("roughness");
      const auto r = [&](const float k0, const float kx, const float ky, const float kxy, const float kxx,
                         const float kyy) {
        return Math("add",
                    Math("add",
                         Math("multiply_add", Math("multiply_add", Const(kxx), x, Const(kx)), x, Const(k0)),
                         Math("multiply", Math("multiply_add", Const(kyy), y, Const(ky)), y)),
                    Math("multiply", Const(kxy), Math("multiply", x, y)));
      };
      albedo = Math("divide",
                    r(13.67300f, -68.78018f, 799.08825f, -905.00061f, 60.28956f, 1086.96473f),
                    r(1.0f, 61.57746f, 442.78211f, 2597.49308f, 121.81241f, 3045.55075f));
    }
    albedo = Math("minimum", Math("maximum", albedo, Const(0.0f)), Const(1.0f));
    return Math("subtract", Const(1.0f), Math("multiply", albedo, input("weight")));
  }

  /* MaterialX 1.39's thin film on a BSDF (nanometres, as Cycles'). */
  template<typename InputFn> void LinkThinFilm(ShaderNode *bsdf, const InputFn &input)
  {
    Link(input("thinfilm_thickness"), In(bsdf, "Thin Film Thickness"));
    Link(input("thinfilm_ior"), In(bsdf, "Thin Film IOR"));
  }

  Val Average(const Val &v)
  {
    if (v.kind != Kind::Vec3 && v.kind != Kind::Vec4) {
      return v;
    }
    if (v.isConstant()) {
      return Const((v.c.x + v.c.y + v.c.z) / 3.0f);
    }
    return VMath("dot_product", v, Const(1.0f / 3.0f), Const(0.0f), true);
  }

  /* MaterialX microfacet roughness is alpha (squared roughness), Cycles uses roughness. */
  /* A Cycles microfacet BSDF's roughness for MaterialX's (alpha along the tangent, alpha along the bitangent):
   * Cycles squares its roughness into the geometric mean of the two alphas and spreads them with its anisotropy,
   * the glossy BSDFs' (Metallic) as alpha / (1 + an), alpha * (1 + an) for an < 0 (1 - an, 1 / (1 - an) above),
   * the Principled BSDF's as alpha / aspect, alpha * aspect with aspect = sqrt(1 - 0.9 an) (0 <= an <= 1; an
   * alpha_y above alpha_x turns the tangent by a quarter turn). Both invert exactly, but for the Principled
   * BSDF's aspects below sqrt(0.1). */
  struct Microfacet {
    Val roughness;
    Val anisotropy;
    Val rotation;
    bool anisotropic = false;
  };

  Microfacet MicrofacetOf(const Val &alpha, const bool principled)
  {
    Microfacet m;
    const Val ax = Component(alpha, 0), ay = Component(alpha, 1);
    if (ax.isConstant() && ay.isConstant()) {
      const float x = std::max(ax.c.x, 0.0f), y = std::max(ay.c.x, 0.0f);
      m.roughness = Const(sqrtf(sqrtf(x * y)));
      m.anisotropic = x != y;
      const float ratio = std::min(x, y) / std::max(std::max(x, y), 1e-8f);
      m.anisotropy = principled ? Const(std::min((1.0f - ratio) / 0.9f, 1.0f)) :
                                  Const((sqrtf(ratio) - 1.0f) * (x > y ? 1.0f : -1.0f));
      m.rotation = Const(principled && y > x ? 0.25f : 0.0f);
      return m;
    }
    m.anisotropic = true;
    m.roughness = Math("sqrt", Math("sqrt", Math("multiply", ax, ay)));
    const Val ratio = Math("divide",
                           Math("minimum", ax, ay),
                           Math("maximum", Math("maximum", ax, ay), Const(1e-8f)));
    if (principled) {
      m.anisotropy = Math("minimum",
                          Math("divide", Math("subtract", Const(1.0f), ratio), Const(0.9f)),
                          Const(1.0f));
      m.rotation = Math("multiply", Math("greater_than", ay, ax), Const(0.25f));
    }
    else {
      m.anisotropy = Math("multiply",
                          Math("subtract", Math("sqrt", ratio), Const(1.0f)),
                          Math("sign", Math("subtract", ax, ay)));
      m.rotation = Const(0.0f);
    }
    return m;
  }

  /* Roughness, anisotropy and (where it is anisotropic) the tangent of a Cycles microfacet BSDF. */
  void LinkMicrofacet(ShaderNode *bsdf, const Val &alpha, const Val &tangent, const bool principled)
  {
    const Microfacet m = MicrofacetOf(alpha, principled);
    Link(m.roughness, In(bsdf, "Roughness"));
    if (!m.anisotropic) {
      return;
    }
    Link(m.anisotropy, In(bsdf, principled ? "Anisotropic" : "Anisotropy"));
    Link(m.rotation, In(bsdf, principled ? "Anisotropic Rotation" : "Rotation"));
    Link(tangent, In(bsdf, "Tangent"));
  }

  /* An isotropic Cycles roughness for MaterialX's alphas (glass: no anisotropy in Cycles). */
  Val RoughnessFromAlpha(const Val &alpha)
  {
    return MicrofacetOf(alpha, false).roughness;
  }

  Val TexCoord()
  {
    ShaderNode *node = Create("texture_coordinate");
    return Of(Out(node, "UV"), Kind::Vec3);
  }

  Val Geometry(const char *output)
  {
    ShaderNode *node = Create("geometry");
    return Of(Out(node, output), Kind::Vec3);
  }

  Val Transform(const Val &v, const char *type, const char *from, const char *to)
  {
    ShaderNode *node = Create("vector_transform");
    SetEnum(node, "transform_type", type);
    SetEnum(node, "convert_from", from);
    SetEnum(node, "convert_to", to);
    Link(Cast(v, Kind::Vec3), In(node, "Vector"));
    return Of(Out(node, "Vector"), Kind::Vec3);
  }

  Val GeomProp(const mx::GeomPropDefPtr &geomProp, const Kind kind)
  {
    if (!geomProp) {
      return Val();
    }
    Val v = GeomProp(geomProp->getGeomProp(), geomProp->getSpace(), kind);
    v.geomDefault = true;
    return v;
  }

  Val GeomProp(const std::string &prop, const std::string &space, const Kind kind)
  {
    const bool object = IsSpace(space, "object");
    Val v;
    if (prop == "texcoord") {
      v = TexCoord();
    }
    else if (prop == "position") {
      v = object ? Of(Out(Create("texture_coordinate"), "Object"), Kind::Vec3) :
                   Geometry("Position");
    }
    else if (prop == "normal") {
      v = Geometry("Normal");
      if (object) {
        v = Transform(v, "normal", "world", "object");
      }
    }
    else if (prop == "tangent" || prop == "bitangent") {
      /* MaterialX's tangent follows the texture coordinates (MaterialX's dPdu, Karma's); Cycles' geometry Tangent
       * is radial around Z (Blender's default for anisotropic BSDFs). */
      ShaderNode *tangent = Create("tangent");
      SetEnum(tangent, "direction_type", "uv_map");
      v = Of(Out(tangent, "Tangent"), Kind::Vec3);
      if (prop == "bitangent") {
        v = VMath("cross_product", Geometry("Normal"), v);
      }
      if (object) {
        v = Transform(v, "vector", "world", "object");
      }
    }
    else if (prop == "viewdirection") {
      /* From the eye to the surface (nprlib's Vworld, facingratio's default). */
      v = Scale(Geometry("Incoming"), Const(-1.0f));
      if (object) {
        v = Transform(v, "vector", "world", "object");
      }
    }
    else {
      ShaderNode *node = Create("attribute");
      SetString(node, "attribute", prop);
      v = Of(Out(node, "Vector"), Kind::Vec3);
    }
    return Cast(v, kind);
  }

  /* MaterialX graph helpers. */

  Val ValueOf(const mx::ValueElementPtr &element, const std::string &type)
  {
    Val v;
    v.kind = KindOf(type);
    if (v.kind == Kind::String) {
      v.str = type == "filename" ? element->getResolvedValueString() : element->getValueString();
      v.colorspace = element->getActiveColorSpace();
      return v;
    }
    const mx::ValuePtr value = element->getType() == type ? element->getValue() :
                                                            mx::Value::createValueFromStrings(
                                                                element->getValueString(), type);
    if (!value) {
      return v;
    }
    if (value->isA<float>()) {
      const float f = value->asA<float>();
      v.c = make_float4(f, f, f, f);
    }
    else if (value->isA<int>()) {
      const float f = float(value->asA<int>());
      v.c = make_float4(f, f, f, f);
    }
    else if (value->isA<bool>()) {
      const float f = value->asA<bool>() ? 1.0f : 0.0f;
      v.c = make_float4(f, f, f, f);
    }
    else if (value->isA<mx::Color3>()) {
      const mx::Color3 c = value->asA<mx::Color3>();
      v.c = make_float4(c[0], c[1], c[2], 1.0f);
    }
    else if (value->isA<mx::Color4>()) {
      const mx::Color4 c = value->asA<mx::Color4>();
      v.c = make_float4(c[0], c[1], c[2], c[3]);
    }
    else if (value->isA<mx::Vector2>()) {
      const mx::Vector2 c = value->asA<mx::Vector2>();
      v.c = make_float4(c[0], c[1], 0.0f, 1.0f);
    }
    else if (value->isA<mx::Vector3>()) {
      const mx::Vector3 c = value->asA<mx::Vector3>();
      v.c = make_float4(c[0], c[1], c[2], 1.0f);
    }
    else if (value->isA<mx::Vector4>()) {
      const mx::Vector4 c = value->asA<mx::Vector4>();
      v.c = make_float4(c[0], c[1], c[2], c[3]);
    }
    else if (value->isA<mx::Matrix33>()) {
      const mx::Matrix33 m = value->asA<mx::Matrix33>();
      MatC c{3, {}};
      for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
          c.m[i][j] = m[i][j];
        }
      }
      return FromMatC(c);
    }
    else if (value->isA<mx::Matrix44>()) {
      const mx::Matrix44 m = value->asA<mx::Matrix44>();
      MatC c{4, {}};
      for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
          c.m[i][j] = m[i][j];
        }
      }
      return FromMatC(c);
    }
    return v;
  }

  /* Value of a node input: its connection or value, or the default of the node definition. */
  Val Input(const mx::NodePtr &node, const std::string &name, const Scope *scope)
  {
    const mx::NodeDefPtr nodeDef = NodeDef(node);
    const mx::InputPtr input = node->getInput(name);
    const mx::InputPtr def = nodeDef ? nodeDef->getActiveInput(name) : nullptr;
    /* The type of the definition, inputs may be authored as another type (strings). */
    const std::string type = def ? def->getType() : input ? input->getType() : std::string();
    if (input) {
      const Val v = Resolve(input, scope, type);
      if (v.kind != Kind::None) {
        return Cast(v, KindOf(type));
      }
    }
    if (!def) {
      return Val();
    }
    if (def->hasDefaultGeomPropString()) {
      return GeomProp(def->getDefaultGeomProp(), KindOf(type));
    }
    return ValueOf(def, type);
  }

  /* Value an input or output refers to: an interface input, an output of a node or node graph,
   * or its own value. */
  Val Resolve(const mx::PortElementPtr &port, const Scope *scope, const std::string &type)
  {
    const mx::InputPtr input = port->asA<mx::Input>();
    if (input && input->hasInterfaceName() && scope) {
      return Input(scope->instance, input->getInterfaceName(), scope->parent);
    }
    if (port->hasNodeGraphString()) {
      const mx::NodeGraphPtr nodeGraph = _doc->getNodeGraph(port->getNodeGraphString());
      if (!nodeGraph) {
        Warn("missing node graph " + port->getNodeGraphString());
        return Val();
      }
      mx::OutputPtr output = port->hasOutputString() ?
                                 nodeGraph->getOutput(port->getOutputString()) :
                                 nullptr;
      if (!output && !nodeGraph->getOutputs().empty()) {
        output = nodeGraph->getOutputs().front();
      }
      return output ? Resolve(output, scope, type) : Val();
    }
    if (port->hasNodeName()) {
      const mx::ElementPtr parent = input ? port->getParent()->getParent() : port->getParent();
      const mx::GraphElementPtr graph = parent ? parent->asA<mx::GraphElement>() : nullptr;
      const mx::NodePtr upstream = graph ? graph->getNode(port->getNodeName()) : nullptr;
      if (!upstream) {
        Warn("missing node " + port->getNodeName());
        return Val();
      }
      return Eval(upstream, port->hasOutputString() ? port->getOutputString() : "out", scope);
    }
    if (input && (input->hasValueString() || KindOf(type) == Kind::String)) {
      return ValueOf(input, type);
    }
    return Val();
  }

  Val Eval(const mx::NodePtr &node, const std::string &output, const Scope *scope)
  {
    const auto key = std::make_tuple(scope, node.get(), output);
    const auto it = _evaluated.find(key);
    if (it != _evaluated.end()) {
      return it->second;
    }
    const Val v = EvalNode(node, output, scope);
    _evaluated[key] = v;
    return v;
  }

  Val EvalNode(const mx::NodePtr &node, const std::string &output, const Scope *scope)
  {
    const std::string &category = node->getCategory();
    const mx::NodeDefPtr nodeDef = NodeDef(node);
    if (!nodeDef) {
      Warn("no MaterialX node definition for node " + node->getName() + " (" + category + ")");
      return Val();
    }
    mx::OutputPtr defOutput = nodeDef->getOutput(output);
    if (!defOutput && !nodeDef->getOutputs().empty()) {
      defOutput = nodeDef->getOutputs().front();
    }
    const std::string outType = defOutput ? defOutput->getType() : node->getType();
    const Kind kind = KindOf(outType);

    const auto input = [&](const char *name) { return Input(node, name, scope); };

    /* Surface shaders map to the Principled BSDF, even when MaterialX defines them as node
     * graphs of its own BSDFs: Cycles has no layering of BSDFs. */
    if (category == "standard_surface") {
      return StandardSurface(input);
    }
    if (category == "open_pbr_surface") {
      return OpenPbrSurface(input);
    }
    if (category == "UsdPreviewSurface") {
      return UsdPreviewSurface(input);
    }
    if (category == "gltf_pbr") {
      return GltfPbr(input);
    }

    /* Nodes defined as node graphs are expanded. */
    const mx::InterfaceElementPtr implementation = nodeDef->getImplementation();
    if (implementation && implementation->isA<mx::NodeGraph>()) {
      const mx::NodeGraphPtr nodeGraph = implementation->asA<mx::NodeGraph>();
      mx::OutputPtr graphOutput = defOutput ? nodeGraph->getOutput(defOutput->getName()) : nullptr;
      if (!graphOutput && !nodeGraph->getOutputs().empty()) {
        graphOutput = nodeGraph->getOutputs().front();
      }
      if (!graphOutput) {
        Warn("node graph of " + nodeDef->getName() + " has no output " + output);
        return Val();
      }
      _scopes.push_back({node, scope});
      return Cast(Resolve(graphOutput, &_scopes.back(), outType), kind);
    }

    bool supported = true;
    Val v = Atomic(category, node, output, kind, outType, input, supported);
    if (!supported) {
      Warn("unsupported MaterialX node " + category + " (" + nodeDef->getName() + ")");
      Val zero;
      zero.kind = kind;
      return zero;
    }
    v = Cast(v, kind);
    return outType == "vector2" ? Vector2(category, v) : v;
  }

  /* A vector2 is a Cycles vector with Z = 0, which Cycles' math does not keep: a float input of a vector2 node is
   * broadcast to Z too (uv * 2 - 1 gave Z = -1), and functions that are not 0 at 0 (cos, exp, invert...) make it
   * nonzero; dot products, lengths and conversions to vector3 then count it (tiledcircles rendered black). */
  Val Vector2(const std::string &category, Val v)
  {
    if (v.isConstant()) {
      v.c.z = 0.0f;
      return v;
    }
    /* Nodes whose Z is 0 already (2D primvars are stored with Z = 0; rotate2d turns about Z). */
    static const std::set<std::string> keepZ = {
        "texcoord", "combine2", "geompropvalue", "geompropvalueuniform", "UsdPrimvarReader", "rotate2d"};
    if (keepZ.count(category) || v.kind != Kind::Vec3) {
      return v;
    }
    return Op(Kind::Vec3, "multiply", "multiply", v, Const(1.0f, 1.0f, 0.0f));
  }

  template<typename InputFn>
  Val Atomic(const std::string &category,
             const mx::NodePtr &node,
             const std::string &output,
             const Kind kind,
             const std::string &outType,
             const InputFn &input,
             bool &supported)
  {
    /* Closures. */
    if (kind == Kind::Closure) {
      return Closure(category, input, supported);
    }
    /* Matrices, and the nodes that take one. */
    if (kind == Kind::Matrix || category == "transformmatrix" || category == "determinant") {
      if (category == "ifgreater" || category == "ifgreatereq" || category == "ifequal" ||
          category == "constant" || category == "dot")
      {
        /* The conditional and organization nodes below handle matrices through Mix. */
      }
      else {
        return MatrixNode(category, node, input, supported);
      }
    }

    /* Math. */
    static const std::map<std::string, std::pair<const char *, const char *>> unary = {
        {"absval", {"absolute", "absolute"}},
        {"floor", {"floor", "floor"}},
        {"ceil", {"ceil", "ceil"}},
        {"round", {"round", "round"}},
        {"sign", {"sign", "sign"}},
        {"sin", {"sine", "sine"}},
        {"cos", {"cosine", "cosine"}},
        {"tan", {"tangent", "tangent"}},
        {"asin", {"arcsine", nullptr}},
        {"acos", {"arccosine", nullptr}},
        {"sqrt", {"sqrt", nullptr}},
        {"fract", {"fraction", "fraction"}},
        {"exp", {"exponent", nullptr}},
    };
    static const std::map<std::string, std::pair<const char *, const char *>> binary = {
        {"add", {"add", "add"}},
        {"subtract", {"subtract", "subtract"}},
        {"multiply", {"multiply", "multiply"}},
        {"divide", {"divide", "divide"}},
        {"modulo", {"floored_modulo", nullptr}},
        {"power", {"power", "power"}},
        {"min", {"minimum", "minimum"}},
        {"max", {"maximum", "maximum"}},
    };
    if (const auto it = unary.find(category); it != unary.end()) {
      return Op(kind, it->second.first, it->second.second, input("in"));
    }
    if (const auto it = binary.find(category); it != binary.end()) {
      return Op(kind, it->second.first, it->second.second, input("in1"), input("in2"));
    }
    if (category == "ln") {
      return Op(kind, "logarithm", nullptr, input("in"), Const(2.718281828f));
    }
    if (category == "atan2") {
      const bool v139 = NodeDef(node)->getActiveInput("iny") != nullptr;
      return Op(
          kind, "arctan2", nullptr, input(v139 ? "iny" : "in1"), input(v139 ? "inx" : "in2"));
    }
    if (category == "clamp") {
      return Op(kind,
                "maximum",
                "maximum",
                Op(kind, "minimum", "minimum", input("in"), input("high")),
                input("low"));
    }
    if (category == "invert") {
      return Op(kind, "subtract", "subtract", input("amount"), input("in"));
    }
    if (category == "mix") {
      return Mix(kind, input("mix"), input("bg"), input("fg"));
    }
    if (category == "remap" || category == "smoothstep") {
      const bool smooth = category == "smoothstep";
      const Val in = input("in");
      const Val low = input(smooth ? "low" : "inlow");
      const Val high = input(smooth ? "high" : "inhigh");
      const Val outLow = smooth ? Const(0.0f) : input("outlow");
      const Val outHigh = smooth ? Const(1.0f) : input("outhigh");
      if (kind == Kind::Float) {
        return MapRange(in, low, high, outLow, outHigh, smooth);
      }
      const Val rgb = VectorMapRange(in, low, high, outLow, outHigh, smooth);
      if (kind == Kind::Vec4) {
        return WithAlpha(rgb,
                         MapRange(Component(in, 3),
                                  Component(low, 3),
                                  Component(high, 3),
                                  Component(outLow, 3),
                                  Component(outHigh, 3),
                                  smooth));
      }
      return rgb;
    }
    /* Cycles' vector math is 3D: the fourth component of a vector4 is added on its own. */
    if (category == "dotproduct") {
      return Dot(input("in1"), input("in2"));
    }
    if (category == "crossproduct") {
      return VMath("cross_product", input("in1"), input("in2"));
    }
    if (category == "normalize") {
      const Val in = input("in");
      if (in.kind != Kind::Vec4) {
        return VMath("normalize", in);
      }
      /* MaterialX's normalize: in / length(in), 0 for a zero vector. */
      const Val length = Math("sqrt", Dot(in, in));
      const Val inverse = Math(
          "divide", Math("greater_than", length, Const(0.0f)), Math("maximum", length, Const(1e-30f)));
      return Op(Kind::Vec4, "multiply", "multiply", in, Cast(inverse, Kind::Vec4));
    }
    if (category == "magnitude") {
      const Val in = input("in");
      return in.kind == Kind::Vec4 ? Math("sqrt", Dot(in, in)) :
                                     VMath("length", in, Const(0.0f), Const(0.0f), true);
    }
    if (category == "dot") {
      return input("in");
    }
    if (category == "constant") {
      return input("value");
    }
    if (category == "convert") {
      return Cast(input("in"), kind);
    }
    if (category == "extract") {
      return Component(input("in"), int(input("index").c.x));
    }
    if (category == "combine2" || category == "combine3" || category == "combine4") {
      const Val in1 = input("in1"), in2 = input("in2");
      if (category == "combine2") {
        if (kind == Kind::Vec4 && in1.kind == Kind::Vec3 && in2.kind == Kind::Vec3) {
          /* Vector4 from two vector2. */
          return WithAlpha(Combine(Component(in1, 0), Component(in1, 1), Component(in2, 0)),
                           Component(in2, 1));
        }
        if (kind == Kind::Vec4) {
          return WithAlpha(Cast(in1, Kind::Vec3), in2);
        }
        return Combine(in1, in2, Const(0.0f));
      }
      const Val rgb = Combine(in1, in2, input("in3"));
      return category == "combine4" ? WithAlpha(rgb, input("in4")) : rgb;
    }
    if (category == "luminance") {
      const Val in = input("in");
      const Val luminance = VMath("dot_product", in, input("lumacoeffs"), Const(0.0f), true);
      return kind == Kind::Vec4 ? WithAlpha(Cast(luminance, Kind::Vec3), Component(in, 3)) :
                                  luminance;
    }
    if (category == "rgbtohsv" || category == "hsvtorgb") {
      const Val in = input("in");
      Val rgb;
      if (category == "rgbtohsv") {
        ShaderNode *separate = Create("separate_color");
        SetEnum(separate, "color_type", "hsv");
        Link(Cast(in, Kind::Vec3), In(separate, "Color"));
        rgb = Combine(Of(Out(separate, "Red"), Kind::Float),
                      Of(Out(separate, "Green"), Kind::Float),
                      Of(Out(separate, "Blue"), Kind::Float));
      }
      else {
        ShaderNode *combine = Create("combine_color");
        SetEnum(combine, "color_type", "hsv");
        /* MaterialX wraps the hue, Cycles expects it in 0..1. */
        Link(Math("fraction", Component(in, 0)), In(combine, "Red"));
        Link(Component(in, 1), In(combine, "Green"));
        Link(Component(in, 2), In(combine, "Blue"));
        rgb = Of(Out(combine, "Color"), Kind::Vec3);
      }
      return kind == Kind::Vec4 ? WithAlpha(rgb, Component(in, 3)) : rgb;
    }
    if (category == "ifgreater" || category == "ifgreatereq" || category == "ifequal") {
      const Val value1 = input("value1"), value2 = input("value2");
      Val fac;
      if (category == "ifgreater") {
        fac = Math("greater_than", value1, value2);
      }
      else if (category == "ifgreatereq") {
        fac = Math("subtract", Const(1.0f), Math("less_than", value1, value2));
      }
      else {
        fac = Math("compare", value1, value2, Const(0.0f));
      }
      if (!NodeDef(node)->getActiveInput("in1")) {
        return fac; /* Boolean result. */
      }
      return Mix(kind, fac, input("in2"), input("in1"));
    }
    if (category == "and") {
      return Math("multiply", input("in1"), input("in2"));
    }
    if (category == "or") {
      return Math("maximum", input("in1"), input("in2"));
    }
    if (category == "not") {
      return Math("subtract", Const(1.0f), input("in"));
    }

    /* Geometry. */
    if (category == "texcoord") {
      return TexCoord();
    }
    if (category == "geomcolor" || category == "geompropvalue" ||
        category == "geompropvalueuniform")
    {
      ShaderNode *attribute = Create("attribute");
      SetString(attribute,
                "attribute",
                category == "geomcolor" ? std::string("displayColor") : input("geomprop").str);
      if (kind == Kind::Float) {
        return Of(Out(attribute, "Fac"), Kind::Float);
      }
      const Val rgb = Of(Out(attribute, outType.rfind("color", 0) == 0 ? "Color" : "Vector"),
                         Kind::Vec3);
      return kind == Kind::Vec4 ? WithAlpha(rgb, Of(Out(attribute, "Alpha"), Kind::Float)) : rgb;
    }
    if (category == "position" || category == "normal" || category == "tangent" ||
        category == "bitangent")
    {
      return GeomProp(category, input("space").str, kind);
    }
    if (category == "time" || category == "frame") {
      ShaderNode *time = Create("scene_time");
      return Of(Out(time, category == "time" ? "Seconds" : "Frame"), Kind::Float);
    }
    if (category == "viewdirection") {
      return GeomProp(category, input("space").str, kind);
    }

    /* Textures. */
    if (category == "image" || category == "hextiledimage") {
      if (category == "hextiledimage") {
        Warn("hex tiled image is rendered as a plain image");
      }
      return Image(input, kind, outType);
    }
    if (category == "hextilednormalmap") {
      /* A plain tangent space normal map of the image: hex tiling (random rotations, scales and offsets per
       * tile, blended) has no Cycles node. */
      Warn("hex tiled normal map is rendered as a plain normal map");
      Val color = Image(input, Kind::Vec3, "vector3");
      const Val flip = input("flip_g");
      if (!(flip.isConstant() && flip.c.x == 0.0f)) {
        const Val g = Component(color, 1);
        color = Combine(Component(color, 0),
                        Mix(Kind::Float, flip, g, Math("subtract", Const(1.0f), g)),
                        Component(color, 2));
      }
      ShaderNode *normalMap = Create("normal_map");
      SetEnum(normalMap, "space", "tangent");
      Link(input("strength"), In(normalMap, "Strength"));
      Link(color, In(normalMap, "Color"));
      return Of(Out(normalMap, "Normal"), Kind::Vec3);
    }
    if (category == "normalmap") {
      const Val in = input("in");
      if (in.bumpNode) {
        /* heighttonormal into normalmap (MaterialX's bump): Cycles' Bump node does both. Decoding its world space
         * normal as a tangent space color turned bumped surfaces nearly black. */
        ShaderInput *distance = In(in.bumpNode, "Distance");
        if (distance->link) {
          _graph->disconnect(distance);
        }
        Link(Math("multiply", *in.bumpScale, Component(input("scale"), 0)), distance);
        const Val normal = input("normal");
        if (!normal.geomDefault) {
          Link(Cast(normal, Kind::Vec3), In(in.bumpNode, "Normal"));
        }
        Val v = in;
        v.bumpNode = nullptr;
        v.bumpScale.reset();
        return v;
      }
      ShaderNode *normalMap = Create("normal_map");
      SetEnum(normalMap, "space", "tangent");
      Link(Component(input("scale"), 0), In(normalMap, "Strength"));
      Link(Cast(input("in"), Kind::Vec3), In(normalMap, "Color"));
      return Of(Out(normalMap, "Normal"), Kind::Vec3);
    }
    if (category == "heighttonormal") {
      ShaderNode *bump = Create("bump");
      Link(Cast(input("in"), Kind::Float), In(bump, "Height"));
      /* Its Distance waits for the normalmap it normally feeds (Val::bumpNode); alone it is its own scale. */
      const Val scale = Cast(input("scale"), Kind::Float);
      Link(scale, In(bump, "Distance"));
      Val v = Of(Out(bump, "Normal"), Kind::Vec3);
      v.bumpNode = bump;
      v.bumpScale = std::make_shared<Val>(scale);
      return v;
    }
    if (category == "noise2d" || category == "noise3d" || category == "fractal3d" ||
        category == "fractal2d")
    {
      const bool is2d = category == "noise2d" || category == "fractal2d";
      ShaderNode *noise = Create("noise_texture");
      SetEnum(noise, "dimensions", is2d ? "2D" : "3D");
      SetEnum(noise, "type", "fBM");
      SetBool(noise, "use_normalize", false); /* Signed noise, like MaterialX. */
      Link(input(is2d ? "texcoord" : "position"), In(noise, "Vector"));
      Link(Const(1.0f), In(noise, "Scale"));
      if (category.rfind("fractal", 0) == 0) {
        Link(Math("subtract", input("octaves"), Const(1.0f)), In(noise, "Detail"));
        Link(input("lacunarity"), In(noise, "Lacunarity"));
        Link(input("diminish"), In(noise, "Roughness"));
      }
      else {
        Link(Const(0.0f), In(noise, "Detail"));
      }
      const Val noiseValue = kind == Kind::Float ? Of(Out(noise, "Fac"), Kind::Float) :
                                                   Of(Out(noise, "Color"), Kind::Vec3);
      Val v = Mul(kind == Kind::Float ? Kind::Float : Kind::Vec3, noiseValue, input("amplitude"));
      if (NodeDef(node)->getActiveInput("pivot")) {
        v = Op(v.kind, "add", "add", v, input("pivot"));
      }
      return v;
    }
    if (category == "cellnoise2d" || category == "cellnoise3d") {
      const bool is2d = category == "cellnoise2d";
      ShaderNode *noise = Create("white_noise_texture");
      SetEnum(noise, "dimensions", is2d ? "2D" : "3D");
      Link(VMath("floor", input(is2d ? "texcoord" : "position")), In(noise, "Vector"));
      return kind == Kind::Float ? Of(Out(noise, "Value"), Kind::Float) :
                                   Of(Out(noise, "Color"), Kind::Vec3);
    }
    if (category == "worleynoise2d" || category == "worleynoise3d") {
      const bool is2d = category == "worleynoise2d";
      ShaderNode *voronoi = Create("voronoi_texture");
      SetEnum(voronoi, "dimensions", is2d ? "2D" : "3D");
      SetEnum(voronoi, "feature", "f1");
      Link(input(is2d ? "texcoord" : "position"), In(voronoi, "Vector"));
      Link(Const(1.0f), In(voronoi, "Scale"));
      Link(input("jitter"), In(voronoi, "Randomness"));
      const bool solid = int(input("style").c.x) == 1;
      if (solid) {
        return Of(Out(voronoi, "Color"), Kind::Vec3);
      }
      return Of(Out(voronoi, "Distance"), Kind::Float);
    }
    if (category == "ramplr" || category == "ramptb" || category == "splitlr" ||
        category == "splittb")
    {
      const bool vertical = category == "ramptb" || category == "splittb";
      const Val coord = Component(input("texcoord"), vertical ? 1 : 0);
      Val fac;
      if (category.rfind("ramp", 0) == 0) {
        fac = Op(
            Kind::Float, "maximum", nullptr, Math("minimum", coord, Const(1.0f)), Const(0.0f));
      }
      else {
        fac = Math("greater_than", coord, input("center"));
      }
      /* MaterialX's top value is at v = 0 (genosl: mix(valuet, valueb, v)), although its v = 0 is the bottom of
       * an image. */
      return vertical ? Mix(kind, fac, input("valuet"), input("valueb")) :
                        Mix(kind, fac, input("valuel"), input("valuer"));
    }

    /* Compositing. */
    static const std::map<std::string, const char *> blends = {
        {"plus", "add"},
        {"minus", "subtract"},
        {"difference", "difference"},
        {"screen", "screen"},
    };
    /* Not Cycles' Burn and Dodge blends (Blender's formulas): MaterialX's per channel
     * mix * (1 - (1 - bg) / fg) and mix * bg / (1 - fg) over (1 - mix) * bg, 0 where the divisor
     * is below M_FLOAT_EPS. */
    if (category == "burn" || category == "dodge") {
      const bool burn = category == "burn";
      const Val fg = input("fg"), bg = input("bg"), mix = input("mix");
      const auto channel = [&](const Val &f, const Val &b) {
        const Val divisor = burn ? f : Math("subtract", Const(1.0f), f);
        const Val zero = Math("less_than", Math("absolute", divisor), Const(1e-8f));
        const Val q = Math("divide",
                           burn ? Math("subtract", Const(1.0f), b) : b,
                           Mix(Kind::Float, zero, divisor, Const(1.0f)));
        const Val blended = Mix(Kind::Float, mix, b, burn ? Math("subtract", Const(1.0f), q) : q);
        return Mix(Kind::Float, zero, blended, Const(0.0f));
      };
      if (kind == Kind::Float) {
        return channel(fg, bg);
      }
      const Val rgb = Combine(channel(Component(fg, 0), Component(bg, 0)),
                              channel(Component(fg, 1), Component(bg, 1)),
                              channel(Component(fg, 2), Component(bg, 2)));
      return kind == Kind::Vec4 ? WithAlpha(rgb, channel(Component(fg, 3), Component(bg, 3))) :
                                  rgb;
    }
    if (const auto it = blends.find(category); it != blends.end()) {
      const Val fg = input("fg"), bg = input("bg"), mix = input("mix");
      const Val rgb = Blend(it->second, mix, bg, fg);
      if (kind == Kind::Float) {
        return Component(rgb, 0);
      }
      if (kind == Kind::Vec4) {
        return WithAlpha(rgb,
                         Component(Blend(it->second, mix, Component(bg, 3), Component(fg, 3)), 0));
      }
      return rgb;
    }
    if (category == "over") {
      const Val fg = input("fg"), bg = input("bg");
      const Val fgAlpha = Component(fg, 3);
      const Val invAlpha = Math("subtract", Const(1.0f), fgAlpha);
      const Val rgb = VMath("add", fg, Scale(bg, invAlpha));
      const Val alpha = Math("multiply_add", Component(bg, 3), invAlpha, fgAlpha);
      return Mix(kind, input("mix"), bg, WithAlpha(rgb, alpha));
    }
    /* MaterialX's merge nodes on premultiplied color4, f and b the fg and bg alphas. */
    if (category == "in" || category == "out" || category == "mask" || category == "matte" ||
        category == "disjointover")
    {
      const Val fg = input("fg"), bg = input("bg");
      const Val f = Component(fg, 3), b = Component(bg, 3);
      Val result;
      if (category == "in" || category == "out") {
        /* fg * b, fg * (1 - b) */
        const Val k = category == "in" ? b : Math("subtract", Const(1.0f), b);
        result = WithAlpha(Scale(fg, k), Math("multiply", f, k));
      }
      else if (category == "mask") {
        /* bg * f */
        result = WithAlpha(Scale(bg, f), Math("multiply", b, f));
      }
      else if (category == "matte") {
        /* fg * f + bg * (1 - f), alpha f + b (1 - f) */
        const Val invF = Math("subtract", Const(1.0f), f);
        result = WithAlpha(VMath("add", Scale(fg, f), Scale(bg, invF)),
                           Math("multiply_add", b, invF, f));
      }
      else {
        /* fg + bg where f + b <= 1, else fg + bg (1 - f) / b; alpha min(f + b, 1) */
        const Val sum = Math("add", f, b);
        const Val over = Math("greater_than", sum, Const(1.0f));
        const Val k = Math("divide", Math("subtract", Const(1.0f), f), b);
        const Val bgScale = Math(
            "multiply_add", over, Math("subtract", k, Const(1.0f)), Const(1.0f));
        result = WithAlpha(VMath("add", fg, Scale(bg, bgScale)),
                           Math("minimum", sum, Const(1.0f)));
      }
      return Mix(kind, input("mix"), bg, result);
    }
    if (category == "premult" || category == "unpremult") {
      const Val in = input("in");
      const Val alpha = Component(in, 3);
      const Val rgb = category == "premult" ? Scale(in, alpha) :
                                              VMath("divide", in, Cast(alpha, Kind::Vec3));
      return WithAlpha(rgb, alpha);
    }
    if (category == "inside" || category == "outside") {
      const Val mask = category == "inside" ? input("mask") :
                                              Math("subtract", Const(1.0f), input("mask"));
      return Mul(kind, input("in"), mask);
    }

    /* Transforms. */
    if (category == "rotate2d" || category == "rotate3d") {
      ShaderNode *rotate = Create("vector_rotate");
      SetEnum(rotate, "rotate_type", category == "rotate2d" ? "z_axis" : "axis");
      /* MaterialX rotates clockwise around the axis. */
      SetBool(rotate, "invert", true);
      Link(Cast(input("in"), Kind::Vec3), In(rotate, "Vector"));
      Link(Math("radians", input("amount")), In(rotate, "Angle"));
      if (category == "rotate3d") {
        Link(input("axis"), In(rotate, "Axis"));
      }
      return Of(Out(rotate, "Vector"), Kind::Vec3);
    }
    if (category == "transformpoint" || category == "transformvector" ||
        category == "transformnormal")
    {
      const char *type = category == "transformpoint"  ? "point" :
                         category == "transformvector" ? "vector" :
                                                         "normal";
      const char *from = IsSpace(input("fromspace").str, "object") ? "object" : "world";
      const char *to = IsSpace(input("tospace").str, "object") ? "object" : "world";
      if (std::string(from) == to) {
        return input("in");
      }
      return Transform(input("in"), type, from, to);
    }

    /* Hair. */
    if (category == "chiang_hair_roughness") {
      /* shortcut: Cycles' TT and TRT roughness scales are fixed at the defaults 0.5 and 2, so
       * scale_TT and scale_TRT are ignored; all three outputs are the R roughness. */
      Val roughness = Combine(input("longitudinal"), input("azimuthal"), Const(0.0f));
      roughness.hairRoughness = true;
      return roughness;
    }
    if (category == "chiang_hair_absorption_from_color") {
      /* (log(clamp(color, 0.001, 1)) / polynomial(azimuthal_roughness))^2 */
      const Val b = input("azimuthal_roughness");
      Val factor = Const(0.245f);
      for (const float k : {5.574f, -10.73f, 2.532f, -0.215f, 5.969f}) {
        factor = Math("multiply_add", factor, b, Const(k));
      }
      const Val color = input("color");
      Val sigma[3];
      for (int i = 0; i < 3; i++) {
        const Val c = Math(
            "minimum", Math("maximum", Component(color, i), Const(0.001f)), Const(1.0f));
        const Val s = Math("divide", Math("logarithm", c, Const(2.718281828f)), factor);
        sigma[i] = Math("multiply", s, s);
      }
      return Combine(sigma[0], sigma[1], sigma[2]);
    }
    if (category == "deon_hair_absorption_from_melanin") {
      /* max(eumelanin * -log(eumelanin_color) + pheomelanin * -log(pheomelanin_color), 0) */
      const Val melanin = Math(
          "multiply",
          Math("logarithm",
               Math("maximum",
                    Math("subtract", Const(1.0f), input("melanin_concentration")),
                    Const(0.0001f)),
               Const(2.718281828f)),
          Const(-1.0f));
      const Val redness = input("melanin_redness");
      const Val eumelanin = Math("multiply", melanin, Math("subtract", Const(1.0f), redness));
      const Val pheomelanin = Math("multiply", melanin, redness);
      const Val eumelaninColor = input("eumelanin_color");
      const Val pheomelaninColor = input("pheomelanin_color");
      Val sigma[3];
      for (int i = 0; i < 3; i++) {
        const Val eu = Math("logarithm", Component(eumelaninColor, i), Const(2.718281828f));
        const Val pheo = Math("logarithm", Component(pheomelaninColor, i), Const(2.718281828f));
        const Val sum = Math(
            "add", Math("multiply", eumelanin, eu), Math("multiply", pheomelanin, pheo));
        sigma[i] = Math("maximum", Math("multiply", sum, Const(-1.0f)), Const(0.0f));
      }
      return Combine(sigma[0], sigma[1], sigma[2]);
    }

    /* Physics. */
    if (category == "artistic_ior") {
      return ArtisticIor(input("reflectivity"), input("edge_color"), output == "extinction");
    }
    if (category == "blackbody") {
      ShaderNode *blackbody = Create("blackbody");
      Link(input("temperature"), In(blackbody, "Temperature"));
      return Of(Out(blackbody, "Color"), Kind::Vec3);
    }
    /* pbrlib's genosl: alpha = clamp(r^2, eps, 1), alpha / aspect (at most 1) and alpha * aspect with aspect =
     * sqrt(1 - clamp(a, 0, 0.98)) for a > 0; roughness_dual squares both, Y < 0 meaning X. */
    if (category == "roughness_anisotropy") {
      const Val r = input("roughness"), a = input("anisotropy");
      const Val alpha = Math("minimum", Math("maximum", Math("multiply", r, r), Const(1e-8f)), Const(1.0f));
      const Val clamped = Math("minimum", Math("maximum", a, Const(0.0f)), Const(0.98f));
      const Val aspect = Math("sqrt", Math("subtract", Const(1.0f), clamped));
      return Combine(Math("minimum", Math("divide", alpha, aspect), Const(1.0f)),
                     Math("multiply", alpha, aspect),
                     Const(0.0f));
    }
    if (category == "roughness_dual") {
      const Val r = input("roughness");
      const auto square = [&](const Val &x) {
        return Math("minimum", Math("maximum", Math("multiply", x, x), Const(1e-8f)), Const(1.0f));
      };
      const Val x = square(Component(r, 0));
      const Val y = Mix(Kind::Float, Math("less_than", Component(r, 1), Const(0.0f)), square(Component(r, 1)), x);
      return Combine(x, y, Const(0.0f));
    }

    (void)output;
    supported = false;
    return Val();
  }

  Val MapRange(const Val &in,
               const Val &low,
               const Val &high,
               const Val &outLow,
               const Val &outHigh,
               const bool smooth)
  {
    ShaderNode *node = Create("map_range");
    SetEnum(node, "range_type", smooth ? "smoothstep" : "linear");
    SetBool(node, "clamp", smooth);
    Link(Cast(in, Kind::Float), In(node, "Value"));
    Link(Cast(low, Kind::Float), In(node, "From Min"));
    Link(Cast(high, Kind::Float), In(node, "From Max"));
    Link(Cast(outLow, Kind::Float), In(node, "To Min"));
    Link(Cast(outHigh, Kind::Float), In(node, "To Max"));
    return Of(Out(node, "Result"), Kind::Float);
  }

  Val VectorMapRange(const Val &in,
                     const Val &low,
                     const Val &high,
                     const Val &outLow,
                     const Val &outHigh,
                     const bool smooth)
  {
    ShaderNode *node = Create("vector_map_range");
    SetEnum(node, "range_type", smooth ? "smoothstep" : "linear");
    SetBool(node, "use_clamp", smooth);
    Link(Cast(in, Kind::Vec3), In(node, "Vector"));
    Link(Cast(low, Kind::Vec3), In(node, "From_Min_FLOAT3"));
    Link(Cast(high, Kind::Vec3), In(node, "From_Max_FLOAT3"));
    Link(Cast(outLow, Kind::Vec3), In(node, "To_Min_FLOAT3"));
    Link(Cast(outHigh, Kind::Vec3), In(node, "To_Max_FLOAT3"));
    return Of(Out(node, "Vector"), Kind::Vec3);
  }

  Val Blend(const char *type, const Val &fac, const Val &bg, const Val &fg)
  {
    ShaderNode *node = Create("mix_color");
    SetEnum(node, "blend_type", type);
    Link(Cast(fac, Kind::Float), In(node, "Factor"));
    Link(Cast(bg, Kind::Vec3), In(node, "A"));
    Link(Cast(fg, Kind::Vec3), In(node, "B"));
    return Of(Out(node, "Result"), Kind::Vec3);
  }

  static std::string CyclesColorSpace(const std::string &colorspace)
  {
    if (colorspace == "srgb_texture" || colorspace == "srgb_displayp3") {
      return u_colorspace_srgb.string();
    }
    if (colorspace == "lin_rec709" || colorspace == "lin_srgb") {
      return u_colorspace_scene_linear_srgb.string();
    }
    if (colorspace == "raw") {
      return u_colorspace_data.string();
    }
    if (colorspace == "acescg" || colorspace == "lin_ap1") {
      return "ACEScg";
    }
    /* Other names are looked up in the OpenColorIO configuration. */
    return colorspace;
  }

  template<typename InputFn>
  Val Image(const InputFn &input, const Kind kind, const std::string &outType)
  {
    const Val file = input("file");
    ShaderNode *image = Create("image_texture");
    SetString(image, "filename", file.str);
    /* MaterialX only color manages color images. Without a color space, detect it from the file
     * like Karma does (8 bit images are sRGB). */
    const bool isColor = outType.rfind("color", 0) == 0;
    const SocketType *colorspace = image->type->find_input(ustring("colorspace"));
    image->set(*colorspace,
               !isColor                ? u_colorspace_data :
               file.colorspace.empty() ? u_colorspace_auto :
                                         ustring(CyclesColorSpace(file.colorspace)));
    const std::string address = input("uaddressmode").str;
    SetEnum(image,
            "extension",
            address == "constant" ? "black" :
            address == "clamp"    ? "clamp" :
            address == "mirror"   ? "mirror" :
                                    "periodic");
    const std::string filter = input("filtertype").str;
    SetEnum(image,
            "interpolation",
            filter == "closest" ? "closest" :
            filter == "cubic"   ? "cubic" :
                                  "linear");
    /* Hex tiled images scale their texture coordinates. */
    const Val tiling = input("tiling");
    Link(tiling.kind == Kind::None ? input("texcoord") : Op(Kind::Vec3, "multiply", "multiply", input("texcoord"), tiling),
         In(image, "Vector"));
    const Val color = Of(Out(image, "Color"), Kind::Vec3);
    switch (kind) {
      case Kind::Float:
        return Component(color, 0);
      case Kind::Vec4:
        return WithAlpha(color, Of(Out(image, "Alpha"), Kind::Float));
      default:
        return color;
    }
  }

  template<typename InputFn>
  Val Closure(const std::string &category, const InputFn &input, bool &supported)
  {
    /* Opaque BSDFs let nothing through to a base below them (throughput 0). */
    if (category == "oren_nayar_diffuse_bsdf" || category == "burley_diffuse_bsdf") {
      ShaderNode *bsdf = Create("diffuse_bsdf");
      Link(Mul(Kind::Vec3, input("color"), input("weight")), In(bsdf, "Color"));
      Link(input("roughness"), In(bsdf, "Roughness"));
      LinkGeom(input("normal"), In(bsdf, "Normal"));
      return WithThroughput(Of(Out(bsdf, "BSDF"), Kind::Closure), Const(0.0f));
    }
    if (category == "translucent_bsdf") {
      ShaderNode *bsdf = Create("translucent_bsdf");
      Link(Mul(Kind::Vec3, input("color"), input("weight")), In(bsdf, "Color"));
      LinkGeom(input("normal"), In(bsdf, "Normal"));
      return WithThroughput(Of(Out(bsdf, "BSDF"), Kind::Closure), Const(0.0f));
    }
    if (category == "dielectric_bsdf") {
      const std::string mode = input("scatter_mode").str;
      const Val throughput = DielectricThroughput(input);
      /* Fresnel weighted reflection: a Principled BSDF without diffuse. */
      const auto reflection = [&]() {
        ShaderNode *bsdf = Create("principled_bsdf");
        Link(Const(0.0f, 0.0f, 0.0f), In(bsdf, "Base Color"));
        Link(input("tint"), In(bsdf, "Specular Tint"));
        Link(input("ior"), In(bsdf, "IOR"));
        LinkMicrofacet(bsdf, input("roughness"), input("tangent"), true);
        LinkGeom(input("normal"), In(bsdf, "Normal"));
        LinkThinFilm(bsdf, input);
        return WithThroughput(Weighted(Of(Out(bsdf, "BSDF"), Kind::Closure), input("weight")), throughput);
      };
      if (mode == "R") {
        return reflection();
      }
      /* Transmission only (T): Cycles' Refraction BSDF, the reflection being another layer's (Lama's dielectric
       * is a reflection over a transmission over its interior). Both (RT): its Glass BSDF. */
      ShaderNode *bsdf = Create(mode == "T" ? "refraction_bsdf" : "glass_bsdf");
      Link(Mul(Kind::Vec3, input("tint"), input("weight")), In(bsdf, "Color"));
      Link(input("ior"), In(bsdf, "IOR"));
      Link(RoughnessFromAlpha(input("roughness")), In(bsdf, "Roughness"));
      LinkGeom(input("normal"), In(bsdf, "Normal"));
      if (mode != "T") {
        LinkThinFilm(bsdf, input);
      }
      Val v = WithThroughput(Of(Out(bsdf, "BSDF"), Kind::Closure), throughput);
      Val none;
      none.kind = Kind::Closure;
      v.opaque = std::make_shared<Val>(mode == "T" ? WithThroughput(none, throughput) : reflection());
      return v;
    }
    if (category == "conductor_bsdf" || category == "generalized_schlick_bsdf") {
      ShaderNode *bsdf = Create("metallic_bsdf");
      if (category == "conductor_bsdf") {
        SetEnum(bsdf, "fresnel_type", "physical_conductor");
        Link(input("ior"), In(bsdf, "IOR"));
        Link(input("extinction"), In(bsdf, "Extinction"));
      }
      else {
        SetEnum(bsdf, "fresnel_type", "f82");
        Link(input("color0"), In(bsdf, "Base Color"));
        Link(input("color90"), In(bsdf, "Edge Tint"));
      }
      LinkMicrofacet(bsdf, input("roughness"), input("tangent"), false);
      LinkGeom(input("normal"), In(bsdf, "Normal"));
      LinkThinFilm(bsdf, input);
      const Val closure = Weighted(Of(Out(bsdf, "BSDF"), Kind::Closure), input("weight"));
      return WithThroughput(closure, category == "conductor_bsdf" ? Const(0.0f) : SchlickThroughput(input));
    }
    if (category == "sheen_bsdf") {
      ShaderNode *bsdf = Create("sheen_bsdf");
      SetEnum(bsdf, "distribution", "microfiber");
      Link(Mul(Kind::Vec3, input("color"), input("weight")), In(bsdf, "Color"));
      Link(input("roughness"), In(bsdf, "Roughness"));
      LinkGeom(input("normal"), In(bsdf, "Normal"));
      return WithThroughput(Of(Out(bsdf, "BSDF"), Kind::Closure), SheenThroughput(input));
    }
    if (category == "subsurface_bsdf") {
      ShaderNode *bssrdf = Create("subsurface_scattering");
      SetEnum(bssrdf, "method", "random_walk");
      Link(Mul(Kind::Vec3, input("color"), input("weight")), In(bssrdf, "Color"));
      Link(input("radius"), In(bssrdf, "Radius"));
      Link(Const(1.0f), In(bssrdf, "Scale"));
      Link(input("anisotropy"), In(bssrdf, "Anisotropy"));
      LinkGeom(input("normal"), In(bssrdf, "Normal"));
      return WithThroughput(Of(Out(bssrdf, "BSSRDF"), Kind::Closure), Const(0.0f));
    }
    if (category == "chiang_hair_bsdf") {
      ShaderNode *bsdf = Create("principled_hair_bsdf");
      SetEnum(bsdf, "model", "Chiang");
      SetEnum(bsdf, "parametrization", "Absorption coefficient");
      Link(input("absorption_coefficient"), In(bsdf, "Absorption Coefficient"));
      Link(input("ior"), In(bsdf, "IOR"));
      /* Cuticle angle 0..1 is -90..90 degrees, Cycles' Offset in radians. */
      Link(Math("multiply_add", input("cuticle_angle"), Const(M_PI_F), Const(-M_PI_2_F)),
           In(bsdf, "Offset"));
      /* shortcut: Cycles derives the TT and TRT roughness from R with the default scales, so
       * roughness_TT and roughness_TRT are ignored. */
      const Val roughness = input("roughness_R");
      if (roughness.hairRoughness) {
        Link(Component(roughness, 0), In(bsdf, "Roughness"));
        Link(Component(roughness, 1), In(bsdf, "Radial Roughness"));
      }
      else if (roughness.isConstant()) {
        Link(Const(HairRoughness(roughness.c.x, false)), In(bsdf, "Roughness"));
        Link(Const(HairRoughness(roughness.c.y, true)), In(bsdf, "Radial Roughness"));
      }
      else {
        Warn("chiang_hair_bsdf roughness_R not from chiang_hair_roughness or constant: ignored");
      }
      /* shortcut: Cycles has no lobe tints, their average weights the whole BSDF. */
      const Val closure = Of(Out(bsdf, "BSDF"), Kind::Closure);
      Val tint = Const(0.0f);
      bool white = true;
      for (const char *name : {"tint_R", "tint_TT", "tint_TRT"}) {
        const Val t = input(name);
        white &= t.isConstant() && t.c.x == 1.0f && t.c.y == 1.0f && t.c.z == 1.0f;
        tint = Math("multiply_add", Average(t), Const(1.0f / 3.0f), tint);
      }
      return WithThroughput(white ? closure : Weighted(closure, tint), Const(0.0f));
    }
    if (category == "dot") {
      return input("in"); /* A shader passed through. */
    }
    /* EDFs: Cycles' emission is uniform; their angular factors are computed per shading point from the view
     * direction (Geometry's Incoming points to the viewer), as MaterialX's OSL does. */
    if (category == "generalized_schlick_edf") {
      /* base * mix(color0, color90, (1 - |N.V|)^exponent) */
      const Val cosTheta = Math("absolute", VMath("dot_product", Geometry("Normal"), Geometry("Incoming"), Const(0.0f), true));
      const Val x = Math("power",
                         Math("minimum", Math("maximum", Math("subtract", Const(1.0f), cosTheta), Const(0.0f)), Const(1.0f)),
                         input("exponent"));
      return ScaleEmission(input("base"), Mix(Kind::Vec3, x, input("color0"), input("color90")));
    }
    if (category == "conical_edf") {
      /* MaterialX's OSL and GLSL have none (its MDL marks its own mapping TODO): the specification's cone around
       * the normal, full inside inner_angle, falling off smoothly to 0 at outer_angle (none when outer <= inner).
       * The angles are from the normal, in degrees. */
      const Val cosTheta = Math("absolute", VMath("dot_product", input("normal"), Geometry("Incoming"), Const(0.0f), true));
      const Val inner = input("inner_angle"), outer = input("outer_angle");
      const auto cosOf = [&](const Val &degrees) {
        return Math("cosine", Math("multiply", degrees, Const(M_PI_F / 180.0f)));
      };
      const Val edge = cosOf(Math("maximum", inner, outer));
      const Val full = cosOf(inner);
      /* outer <= inner: a hard edge at inner. */
      const Val soft = Math("greater_than", outer, inner);
      const Val start = Mix(Kind::Float, soft, Math("subtract", full, Const(1e-4f)), edge);
      ShaderNode *range = Create("map_range");
      SetEnum(range, "range_type", "smoothstep");
      SetBool(range, "clamp", true);
      Link(cosTheta, In(range, "Value"));
      Link(start, In(range, "From Min"));
      Link(full, In(range, "From Max"));
      Link(Const(0.0f), In(range, "To Min"));
      Link(Const(1.0f), In(range, "To Max"));
      ShaderNode *emission = Create("emission");
      Link(Op(Kind::Vec3, "multiply", "multiply", input("color"), Cast(Of(Out(range, "Result"), Kind::Float), Kind::Vec3)),
           In(emission, "Color"));
      Link(Const(1.0f), In(emission, "Strength"));
      return Of(Out(emission, "Emission"), Kind::Closure);
    }
    if (category == "measured_edf") {
      Warn("measured_edf is rendered as uniform emission (its IES profile is not read)");
      ShaderNode *emission = Create("emission");
      Link(input("color"), In(emission, "Color"));
      Link(Const(1.0f), In(emission, "Strength"));
      return Of(Out(emission, "Emission"), Kind::Closure);
    }
    if (category == "uniform_edf") {
      ShaderNode *emission = Create("emission");
      Link(input("color"), In(emission, "Color"));
      Link(Const(1.0f), In(emission, "Strength"));
      return Of(Out(emission, "Emission"), Kind::Closure);
    }
    /* Volumes: MaterialX's VDFs are coefficients per unit length, like Cycles' Volume
     * Coefficients node; the volume's EDF is volume emission. */
    if (category == "anisotropic_vdf" || category == "absorption_vdf") {
      ShaderNode *volume = Create("volume_coefficients");
      Link(input("absorption"), In(volume, "Absorption Coefficients"));
      if (category == "anisotropic_vdf") {
        Link(input("scattering"), In(volume, "Scatter Coefficients"));
        Link(input("anisotropy"), In(volume, "Anisotropy"));
      }
      else {
        Link(Const(0.0f, 0.0f, 0.0f), In(volume, "Scatter Coefficients"));
      }
      Link(Const(0.0f, 0.0f, 0.0f), In(volume, "Emission Coefficients"));
      return Of(Out(volume, "Volume"), Kind::Closure);
    }
    if (category == "volume") {
      return AddClosures(input("vdf"), input("edf"));
    }
    if (category == "volumematerial") {
      return input("volumeshader");
    }
    /* Layering as MaterialX's GLSL (Storm): the base under the top's throughput, the throughputs combined. */
    if (category == "layer") {
      const Val top = input("top"), base = input("base");
      if (base.out && base.out->parent->type->name.string().find("volume") != std::string::npos) {
        /* A BSDF over a VDF (a surface over its interior): Cycles' surface and volume closures are separate,
         * the sum keeps both; over an opaque base the interior is not seen. */
        Val v = WithThroughput(AddClosures(top, base), ThroughputOf(top));
        v.opaque = std::make_shared<Val>(OpaqueOf(top));
        return v;
      }
      /* Over a BSDF base the top's transmission goes into the base: the top without it. */
      const Val t = ThroughputOf(top);
      const Val throughput = Math("multiply", t, ThroughputOf(base));
      Val v = WithThroughput(AddClosures(OpaqueOf(top), Weighted(base, t)), throughput);
      if (base.opaque) {
        v.opaque = std::make_shared<Val>(
            WithThroughput(AddClosures(OpaqueOf(top), Weighted(*base.opaque, t)), throughput));
      }
      return v;
    }
    if (category == "add") {
      const Val a = input("in1"), b = input("in2");
      const Val throughput = Math(
          "maximum", Math("subtract", Math("add", ThroughputOf(a), ThroughputOf(b)), Const(1.0f)), Const(0.0f));
      Val v = WithThroughput(AddClosures(a, b), throughput);
      if (a.opaque || b.opaque) {
        v.opaque = std::make_shared<Val>(WithThroughput(AddClosures(OpaqueOf(a), OpaqueOf(b)), throughput));
      }
      return v;
    }
    if (category == "mix") {
      const Val mix = input("mix"), bg = input("bg"), fg = input("fg");
      const Val throughput = Mix(Kind::Float, Cast(mix, Kind::Float), ThroughputOf(bg), ThroughputOf(fg));
      Val v = WithThroughput(Mix(Kind::Closure, mix, bg, fg), throughput);
      if (bg.opaque || fg.opaque) {
        v.opaque = std::make_shared<Val>(
            WithThroughput(Mix(Kind::Closure, mix, OpaqueOf(bg), OpaqueOf(fg)), throughput));
      }
      return v;
    }
    if (category == "multiply") {
      const Val closure = input("in1"), weight = input("in2");
      /* An emission's colour can be scaled exactly; other closures by the weight's average (Cycles mixes
       * closures by a float). */
      if (weight.kind != Kind::Float && IsEmission(closure)) {
        return ScaleEmission(closure, weight);
      }
      Val v = WithThroughput(Weighted(closure, Average(weight)), ThroughputOf(closure));
      if (closure.opaque) {
        v.opaque = std::make_shared<Val>(
            WithThroughput(Weighted(*closure.opaque, Average(weight)), ThroughputOf(closure)));
      }
      return v;
    }
    if (category == "surface") {
      return Opacity(AddClosures(input("bsdf"), input("edf")), input("opacity"));
    }
    if (category == "surface_unlit") {
      ShaderNode *emission = Create("emission");
      Link(input("emission_color"), In(emission, "Color"));
      Link(input("emission"), In(emission, "Strength"));
      return Opacity(Of(Out(emission, "Emission"), Kind::Closure), input("opacity"));
    }
    if (category == "displacement") {
      const Val displacement = input("displacement");
      if (displacement.isConstant() && displacement.c.x == 0.0f && displacement.c.y == 0.0f &&
          displacement.c.z == 0.0f)
      {
        return Val();
      }
      if (displacement.kind == Kind::Float) {
        ShaderNode *node = Create("displacement");
        SetEnum(node, "space", "object");
        Link(displacement, In(node, "Height"));
        Link(Const(0.0f), In(node, "Midlevel"));
        Link(input("scale"), In(node, "Scale"));
        return Of(Out(node, "Displacement"), Kind::Closure);
      }
      ShaderNode *node = Create("vector_displacement");
      SetEnum(node, "space", "object");
      Link(displacement, In(node, "Vector"));
      Link(Const(0.0f), In(node, "Midlevel"));
      Link(input("scale"), In(node, "Scale"));
      return Of(Out(node, "Displacement"), Kind::Closure);
    }
    supported = false;
    return Val();
  }

  Val Opacity(const Val &closure, const Val &opacity)
  {
    const Val fac = Average(opacity);
    if (fac.isConstant() && fac.c.x >= 1.0f) {
      return closure;
    }
    ShaderNode *transparent = Create("transparent_bsdf");
    Link(Const(1.0f, 1.0f, 1.0f), In(transparent, "Color"));
    return Mix(Kind::Closure, fac, Of(Out(transparent, "BSDF"), Kind::Closure), closure);
  }

  static bool IsZero(const Val &v)
  {
    return v.isConstant() && v.c.x == 0.0f;
  }

  /* Principled BSDF inputs set by the surface shader translations. */
  struct Principled {
    Val baseColor, metallic, roughness, ior, alpha, normal, diffuseRoughness;
    Val subsurfaceWeight, subsurfaceRadius, subsurfaceScale, subsurfaceAnisotropy;
    Val specularLevel, specularTint, anisotropic, anisotropicRotation, tangent;
    Val transmissionWeight;
    Val coatWeight, coatRoughness, coatIor, coatTint, coatNormal;
    Val sheenWeight, sheenRoughness, sheenTint;
    Val emissionColor, emissionStrength;
    Val thinFilmThickness, thinFilmIor, thinWall;
  };

  Val CreatePrincipled(const Principled &p)
  {
    ShaderNode *bsdf = Create("principled_bsdf");
    SetEnum(bsdf, "subsurface_method", "random_walk");
    const auto set = [&](const Val &v, const char *name) { Link(v, In(bsdf, name)); };
    set(p.baseColor, "Base Color");
    set(p.metallic, "Metallic");
    set(p.roughness, "Roughness");
    set(p.ior, "IOR");
    set(p.alpha, "Alpha");
    LinkGeom(p.normal, In(bsdf, "Normal"));
    set(p.diffuseRoughness, "Diffuse Roughness");
    set(p.subsurfaceWeight, "Subsurface Weight");
    set(p.subsurfaceRadius, "Subsurface Radius");
    set(p.subsurfaceScale, "Subsurface Scale");
    set(p.subsurfaceAnisotropy, "Subsurface Anisotropy");
    set(p.specularLevel, "Specular IOR Level");
    set(p.specularTint, "Specular Tint");
    set(p.anisotropic, "Anisotropic");
    set(p.anisotropicRotation, "Anisotropic Rotation");
    /* MaterialX's tangent (the UV map's) only where the highlight is anisotropic: Cycles computes a UV tangent for
     * every mesh using the material. */
    if (!(p.anisotropic.isConstant() && p.anisotropic.c.x == 0.0f)) {
      Link(p.tangent, In(bsdf, "Tangent"));
    }
    set(p.transmissionWeight, "Transmission Weight");
    set(p.coatWeight, "Coat Weight");
    set(p.coatRoughness, "Coat Roughness");
    set(p.coatIor, "Coat IOR");
    set(p.coatTint, "Coat Tint");
    LinkGeom(p.coatNormal, In(bsdf, "Coat Normal"));
    set(p.sheenWeight, "Sheen Weight");
    set(p.sheenRoughness, "Sheen Roughness");
    set(p.sheenTint, "Sheen Tint");
    set(p.emissionColor, "Emission Color");
    set(p.emissionStrength, "Emission Strength");
    set(p.thinFilmThickness, "Thin Film Thickness");
    set(p.thinFilmIor, "Thin Film IOR");
    set(p.thinWall, "Thin Wall");
    return Of(Out(bsdf, "BSDF"), Kind::Closure);
  }

  /* The Principled BSDF's anisotropy for an aspect of sqrt(1 - a) (Standard Surface's and OpenPBR's): its own is
   * sqrt(1 - 0.9 an), the same up to a = 0.9. */
  Val AnisotropyOf(const Val &a)
  {
    if (a.isConstant()) {
      return Const(std::clamp(a.c.x / 0.9f, 0.0f, 1.0f));
    }
    return Math("minimum", Math("maximum", Math("divide", a, Const(0.9f)), Const(0.0f)), Const(1.0f));
  }

  /* The Principled BSDF uses the base color for subsurface scattering and transmission. */
  Val BaseColor(Val color,
                const Val &subsurfaceWeight,
                const Val &subsurfaceColor,
                const Val &transmissionWeight,
                const Val &transmissionColor)
  {
    if (!IsZero(subsurfaceWeight)) {
      color = Mix(Kind::Vec3, subsurfaceWeight, color, subsurfaceColor);
    }
    if (!IsZero(transmissionWeight)) {
      color = Mix(Kind::Vec3, transmissionWeight, color, transmissionColor);
    }
    return color;
  }

  template<typename InputFn> Val StandardSurface(const InputFn &input)
  {
    Principled p;
    p.baseColor = BaseColor(Mul(Kind::Vec3, input("base_color"), input("base")),
                            input("subsurface"),
                            input("subsurface_color"),
                            input("transmission"),
                            input("transmission_color"));
    p.metallic = input("metalness");
    p.diffuseRoughness = input("diffuse_roughness");
    p.roughness = input("specular_roughness");
    p.ior = input("specular_IOR");
    p.specularLevel = Math("multiply", input("specular"), Const(0.5f));
    p.specularTint = input("specular_color");
    /* Standard Surface's roughness_anisotropy: alpha / aspect, alpha * aspect with aspect = sqrt(1 - a), the
     * Principled BSDF's aspect is sqrt(1 - 0.9 an). */
    p.anisotropic = AnisotropyOf(input("specular_anisotropy"));
    p.anisotropicRotation = input("specular_rotation");
    p.transmissionWeight = input("transmission");
    p.subsurfaceWeight = input("subsurface");
    p.subsurfaceRadius = input("subsurface_radius");
    p.subsurfaceScale = input("subsurface_scale");
    p.subsurfaceAnisotropy = input("subsurface_anisotropy");
    p.sheenWeight = input("sheen");
    p.sheenTint = input("sheen_color");
    p.sheenRoughness = input("sheen_roughness");
    p.coatWeight = input("coat");
    p.coatTint = input("coat_color");
    p.coatRoughness = input("coat_roughness");
    p.coatIor = input("coat_IOR");
    p.coatNormal = input("coat_normal");
    p.emissionStrength = input("emission");
    p.emissionColor = input("emission_color");
    p.thinFilmThickness = input("thin_film_thickness");
    p.thinFilmIor = input("thin_film_IOR");
    p.alpha = Average(input("opacity"));
    p.thinWall = input("thin_walled");
    p.normal = input("normal");
    p.tangent = input("tangent");
    return CreatePrincipled(p);
  }

  template<typename InputFn> Val OpenPbrSurface(const InputFn &input)
  {
    Principled p;
    const Val baseColor = input("base_color");
    p.baseColor = BaseColor(Mul(Kind::Vec3, baseColor, input("base_weight")),
                            input("subsurface_weight"),
                            input("subsurface_color"),
                            input("transmission_weight"),
                            input("transmission_color"));
    const Val coatWeight = input("coat_weight");
    if (!IsZero(coatWeight)) {
      /* Darkening of the base under the coat by internal reflections, as in the OpenPBR node
       * graph: (1 - Kcoat) / (1 - Ebase * Kcoat), which the Principled BSDF does not have. */
      const Val coatIor = input("coat_ior");
      const Val f0Sqrt = Math(
          "divide", Math("subtract", coatIor, Const(1.0f)), Math("add", coatIor, Const(1.0f)));
      const Val f0 = Math("multiply", f0Sqrt, f0Sqrt);
      const Val kCoat = Math(
          "subtract",
          Const(1.0f),
          Math("divide", Math("subtract", Const(1.0f), f0), Math("multiply", coatIor, coatIor)));
      const Val eDielectric = Mix(
          Kind::Vec3, input("subsurface_weight"), baseColor, input("subsurface_color"));
      const Val eBase = Mix(Kind::Vec3,
                            input("base_metalness"),
                            eDielectric,
                            Mul(Kind::Vec3, baseColor, input("specular_weight")));
      const Val darkening = VMath("divide",
                                  Cast(Math("subtract", Const(1.0f), kCoat), Kind::Vec3),
                                  VMath("subtract", Const(1.0f), Scale(eBase, kCoat)));
      const Val fac = Math("multiply", coatWeight, input("coat_darkening"));
      p.baseColor = Mul(Kind::Vec3, p.baseColor, Mix(Kind::Vec3, fac, Const(1.0f), darkening));
    }
    p.metallic = input("base_metalness");
    p.diffuseRoughness = input("base_diffuse_roughness");
    /* OpenPBR's anisotropy: alpha_x = r^2 sqrt(2 / (1 + (1 - a)^2)), alpha_y = (1 - a) alpha_x. Their geometric
     * mean, Cycles' squared roughness, is r^2 sqrt(2 (1 - a) / (1 + (1 - a)^2)), their aspect sqrt(1 - a) as
     * Standard Surface's. */
    {
      const Val a = input("specular_roughness_anisotropy");
      const Val inverse = Math("subtract", Const(1.0f), Math("minimum", Math("maximum", a, Const(0.0f)), Const(1.0f)));
      const Val spread = Math("divide",
                              Math("multiply", Const(2.0f), inverse),
                              Math("multiply_add", inverse, inverse, Const(1.0f)));
      p.roughness = a.isConstant() && a.c.x == 0.0f ?
                        input("specular_roughness") :
                        Math("multiply", input("specular_roughness"), Math("sqrt", Math("sqrt", spread)));
      p.anisotropic = AnisotropyOf(a);
    }
    p.ior = input("specular_ior");
    p.specularLevel = Math("multiply", input("specular_weight"), Const(0.5f));
    p.specularTint = input("specular_color");
    p.transmissionWeight = input("transmission_weight");
    p.subsurfaceWeight = input("subsurface_weight");
    p.subsurfaceRadius = Mul(
        Kind::Vec3, input("subsurface_radius_scale"), input("subsurface_radius"));
    p.subsurfaceScale = Const(1.0f);
    p.subsurfaceAnisotropy = input("subsurface_scatter_anisotropy");
    p.sheenWeight = input("fuzz_weight");
    p.sheenTint = input("fuzz_color");
    p.sheenRoughness = input("fuzz_roughness");
    p.coatWeight = input("coat_weight");
    p.coatTint = input("coat_color");
    p.coatRoughness = input("coat_roughness");
    p.coatIor = input("coat_ior");
    p.coatNormal = input("geometry_coat_normal");
    p.emissionStrength = input("emission_luminance");
    p.emissionColor = input("emission_color");
    /* Micrometers in OpenPBR, nanometers in Cycles. */
    p.thinFilmThickness = Math("multiply",
                               Math("multiply", input("thin_film_thickness"), Const(1000.0f)),
                               input("thin_film_weight"));
    p.thinFilmIor = input("thin_film_ior");
    p.alpha = input("geometry_opacity");
    p.thinWall = input("geometry_thin_walled");
    p.normal = input("geometry_normal");
    p.tangent = input("geometry_tangent");
    return CreatePrincipled(p);
  }

  template<typename InputFn> Val UsdPreviewSurface(const InputFn &input)
  {
    Principled p;
    p.baseColor = input("diffuseColor");
    p.metallic = input("metallic");
    p.roughness = input("roughness");
    p.ior = input("ior");
    p.coatWeight = input("clearcoat");
    p.coatRoughness = input("clearcoatRoughness");
    p.emissionColor = input("emissiveColor");
    p.emissionStrength = Const(1.0f);
    p.alpha = input("opacity");
    p.normal = input("normal");
    return CreatePrincipled(p);
  }

  template<typename InputFn> Val GltfPbr(const InputFn &input)
  {
    Principled p;
    p.baseColor = input("base_color");
    p.metallic = input("metallic");
    p.roughness = input("roughness");
    p.ior = input("ior");
    p.transmissionWeight = input("transmission");
    p.specularLevel = Math("multiply", input("specular"), Const(0.5f));
    p.specularTint = input("specular_color");
    p.sheenWeight = Const(1.0f);
    p.sheenTint = input("sheen_color");
    p.sheenRoughness = input("sheen_roughness");
    p.coatWeight = input("clearcoat");
    p.coatRoughness = input("clearcoat_roughness");
    p.coatNormal = input("clearcoat_normal");
    p.emissionColor = input("emissive");
    p.emissionStrength = input("emissive_strength");
    /* Alpha mode 0 is opaque. */
    p.alpha = IsZero(input("alpha_mode")) ? Const(1.0f) : input("alpha");
    p.normal = input("normal");
    p.tangent = input("tangent");
    return CreatePrincipled(p);
  }
};

/* The node of the document that the terminal connects to. */
/* The terminal node of the document, by the name of its USD shader. HdMtlx names the material node
 * after that shader and renames the shader itself when the shader and its material have the same
 * name (usdMtlx's looks: Materials/Bishop_B/Bishop_B), so a material node found by the name stands
 * for the shader connected to it. */
mx::NodePtr FindTerminalNode(const mx::DocumentPtr &doc, const std::string &name)
{
  if (mx::NodePtr node = doc->getNode(name)) {
    if (node->getType() != mx::MATERIAL_TYPE_STRING) {
      return node;
    }
  }
  for (const mx::NodePtr &material : doc->getMaterialNodes()) {
    for (const mx::InputPtr &input : material->getInputs()) {
      if (mx::NodePtr node = input->getConnectedNode()) {
        return node;
      }
    }
  }
  return nullptr;
}

/* HdMtlx writes a file input's authored path into the document (Storm resolves its textures on its
 * own), so a relative path ("./wood.png") was looked up in the working directory and rendered
 * Cycles' missing-image magenta. Hydra hands over the resolved path too: put it in its place
 * first. Houdini's paths are mostly absolute, Blender's (USD Nodes') relative. */
void ResolveAssetPaths(HdDataSourceMaterialNetworkInterface &networkInterface)
{
  for (const TfToken &node : networkInterface.GetNodeNames()) {
    for (const TfToken &param : networkInterface.GetAuthoredNodeParameterNames(node)) {
      const VtValue value = networkInterface.GetNodeParameterValue(node, param);
      if (!value.IsHolding<SdfAssetPath>()) {
        continue;
      }
      const SdfAssetPath &path = value.UncheckedGet<SdfAssetPath>();
      const std::string &resolved = path.GetResolvedPath();
      if (!resolved.empty() && resolved != path.GetAssetPath()) {
        networkInterface.SetNodeParameterValue(
            node, param, VtValue(SdfAssetPath(resolved, resolved)));
      }
    }
  }
}

/* The MaterialX document of a terminal node and its upstream nodes, and the terminal's node in
 * it. */
mx::NodePtr CreateDocument(HdDataSourceMaterialNetworkInterface &networkInterface,
                           const mx::DocumentPtr &libraries,
                           const SdfPath &materialId,
                           const TfToken &terminalNode,
                           const TfToken &terminalName,
                           mx::DocumentPtr &doc)
{
  static const bool debug = TfGetenvBool("HD_CYCLES_MTLX_DEBUG", false);
  ResolveAssetPaths(networkInterface);
  try {
    doc = HdMtlxCreateMtlxDocumentFromHdMaterialNetworkInterface(
        &networkInterface,
        terminalNode,
        networkInterface.GetNodeInputConnectionNames(terminalNode),
        libraries);
  }
  catch (const mx::Exception &e) {
    TF_WARN("Could not create a MaterialX document for %s: %s", materialId.GetText(), e.what());
    return nullptr;
  }
  if (!doc) {
    return nullptr;
  }
  if (debug) {
    mx::XmlWriteOptions options;
    options.writeXIncludeEnable = false;
    options.elementPredicate = [](const mx::ConstElementPtr &element) {
      return !element->hasSourceUri();
    };
    fprintf(stderr,
            "[hdCycles] MaterialX document for %s %s:\n%s\n",
            materialId.GetText(),
            terminalName.GetText(),
            mx::writeToXmlString(doc, &options).c_str());
  }

  const mx::NodePtr node = FindTerminalNode(doc, HdMtlxCreateNameFromPath(SdfPath(terminalNode)));
  if (!node) {
    TF_WARN("No MaterialX terminal node for %s %s", materialId.GetText(), terminalName.GetText());
  }
  return node;
}

}  // namespace

const TfTokenVector &MaterialXRenderContexts()
{
  static const TfTokenVector contexts = {MaterialXTokens->mtlx, MaterialXTokens->kma};
  return contexts;
}

bool IsMaterialXNetwork(const HdMaterialNetworkSchema &network)
{
  HdMaterialNodeContainerSchema nodes = network.GetNodes();
  HdMaterialConnectionContainerSchema terminals = network.GetTerminals();
  for (const TfToken &terminalName : terminals.GetNames()) {
    HdMaterialConnectionSchema terminal = terminals.Get(terminalName);
    if (!terminal.GetUpstreamNodePath()) {
      continue;
    }
    HdMaterialNodeSchema node = nodes.Get(terminal.GetUpstreamNodePath()->GetTypedValue(0.0f));
    if (node && node.GetNodeIdentifier() &&
        TfStringStartsWith(node.GetNodeIdentifier()->GetTypedValue(0.0f).GetString(), "ND_"))
    {
      return true;
    }
  }
  return false;
}

unique_ptr<ShaderGraph> BuildMaterialXShaderGraph(const SdfPath &materialId,
                                                  const HdMaterialNetworkSchema &network,
                                                  const HdContainerDataSourceHandle &primContainer)
{
  const mx::DocumentPtr &libraries = HdMtlxStdLibraries();
  if (!libraries) {
    TF_WARN("MaterialX libraries not found, can not translate %s", materialId.GetText());
    return nullptr;
  }

  HdDataSourceMaterialNetworkInterface networkInterface(
      materialId, network.GetContainer(), primContainer);

  unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();
  bool anyLinked = false;

  for (const TfToken &terminalName : networkInterface.GetTerminalNames()) {
    const char *graphInput = terminalName == MaterialXTokens->surface      ? "Surface" :
                             terminalName == MaterialXTokens->displacement ? "Displacement" :
                             terminalName == MaterialXTokens->volume       ? "Volume" :
                                                                             nullptr;
    if (!graphInput) {
      continue;
    }
    const HdMaterialNetworkInterface::InputConnectionResult connection =
        networkInterface.GetTerminalConnection(terminalName);
    if (!connection.first) {
      continue;
    }
    mx::DocumentPtr doc;
    const mx::NodePtr node = CreateDocument(networkInterface,
                                            libraries,
                                            materialId,
                                            connection.second.upstreamNodeName,
                                            terminalName,
                                            doc);
    if (!node) {
      continue;
    }

    Translator translator(graph.get(), doc, materialId);
    const Val result = translator.Eval(node, "out");
    if (result.out) {
      graph->connect(result.out, graph->output()->input(graphInput));
      anyLinked = true;
    }
  }

  if (!anyLinked) {
    return nullptr;
  }
  return graph;
}

struct MaterialXLinkCache {
  /* Document node and its translator (which caches the outputs) per network node, no
   * translator when the node has no document. */
  std::map<TfToken, std::pair<mx::NodePtr, std::unique_ptr<Translator>>> nodes;
};

bool LinkMaterialXNode(ShaderGraph *graph,
                       const SdfPath &materialId,
                       const HdMaterialNetworkSchema &network,
                       const HdContainerDataSourceHandle &primContainer,
                       const TfToken &nodeName,
                       const TfToken &outputName,
                       ShaderInput *input,
                       std::shared_ptr<MaterialXLinkCache> &cache)
{
  const mx::DocumentPtr &libraries = HdMtlxStdLibraries();
  if (!libraries) {
    return false;
  }
  if (!cache) {
    cache = std::make_shared<MaterialXLinkCache>();
  }
  auto [it, inserted] = cache->nodes.try_emplace(nodeName);
  auto &[node, translator] = it->second;
  if (inserted) {
    HdDataSourceMaterialNetworkInterface networkInterface(
        materialId, network.GetContainer(), primContainer);
    mx::DocumentPtr doc;
    node = CreateDocument(networkInterface, libraries, materialId, nodeName, nodeName, doc);
    if (node) {
      translator = make_unique<Translator>(graph, doc, materialId);
    }
  }
  if (!translator) {
    return false;
  }
  return translator->LinkOutput(node, outputName.IsEmpty() ? "out" : outputName.GetString(), input);
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
