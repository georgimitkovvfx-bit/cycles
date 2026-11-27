/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Object Primitive
 *
 * All mesh and curve primitives are part of an object. The same mesh and curves
 * may be instanced multiple times by different objects.
 *
 * If the mesh is not instanced multiple times, the object will not be explicitly
 * stored as a primitive in the BVH, rather the bare triangles are curved are
 * directly primitives in the BVH with world space locations applied, and the object
 * ID is looked up afterwards. */

#pragma once

#include "kernel/globals.h"
#include "kernel/types.h"

#include "util/hash.h"

CCL_NAMESPACE_BEGIN

/* Object attributes, for now a fixed size and contents */

enum ObjectTransform {
  OBJECT_TRANSFORM = 0,
  OBJECT_INVERSE_TRANSFORM = 1,
};

enum ObjectVectorTransform { OBJECT_PASS_MOTION_PRE = 0, OBJECT_PASS_MOTION_POST = 1 };

/* Object to world space transformation */

ccl_device_inline Transform object_fetch_transform(KernelGlobals kg,
                                                   const int object,
                                                   enum ObjectTransform type)
{
  if (type == OBJECT_INVERSE_TRANSFORM) {
    return kernel_data_fetch(objects, object).itfm;
  }
  return kernel_data_fetch(objects, object).tfm;
}

/* Object to world space transformation for motion vectors */

ccl_device_inline Transform object_fetch_motion_pass_transform(KernelGlobals kg,
                                                               const int object,
                                                               enum ObjectVectorTransform type)
{
  const int offset = object * OBJECT_MOTION_PASS_SIZE + (int)type;
  return kernel_data_fetch(object_motion_pass, offset);
}

/* Motion blurred object transformations */

#ifdef __OBJECT_MOTION__
ccl_device_inline Transform object_fetch_transform_motion(KernelGlobals kg,
                                                          const int object,
                                                          const float time)
{
  const uint motion_offset = kernel_data_fetch(objects, object).motion_offset;
  const ccl_global DecomposedTransform *motion = &kernel_data_fetch(object_motion, motion_offset);
  const int num_steps = kernel_data_fetch(objects, object).num_tfm_steps;

  Transform tfm;
  transform_motion_array_interpolate(&tfm, motion, num_steps, time);

  return tfm;
}
#endif /* __OBJECT_MOTION__ */

ccl_device_inline Transform object_fetch_transform_motion_test(KernelGlobals kg,
                                                               const int object,
                                                               const float time,
                                                               ccl_private Transform *itfm)
{
#ifdef __OBJECT_MOTION__
  const uint object_flag = kernel_data_fetch(object_flag, object);
  if (object_flag & SD_OBJECT_MOTION) {
    /* if we do motion blur */
    Transform tfm = object_fetch_transform_motion(kg, object, time);

    if (itfm) {
      *itfm = transform_inverse(tfm);
    }

    return tfm;
  }

#endif /* __OBJECT_MOTION__ */

  Transform tfm = object_fetch_transform(kg, object, OBJECT_TRANSFORM);
  if (itfm) {
    *itfm = object_fetch_transform(kg, object, OBJECT_INVERSE_TRANSFORM);
  }

  return tfm;
}

/* Nested instancing (USD Nodes ADR 0082 › E2): a placement of an object placed by instance arrays. */

#ifdef __INSTANCE_NESTING__
/* The element of a level a placement is in. */
ccl_device_inline uint object_instance_element(const KernelInstanceLevel level, const uint instance)
{
  return (instance / level.stride) % level.count;
}

/* A level's element transform at a time: interpolated between its motion steps when it moves. */
ccl_device_inline Transform object_instance_level_transform(KernelGlobals kg,
                                                            const KernelInstanceLevel level,
                                                            const uint element,
                                                            const float time)
{
#  ifdef __OBJECT_MOTION__
  if (level.num_steps > 1) {
    const ccl_global DecomposedTransform *motion = &kernel_data_fetch(
        instance_motion, level.motion_offset + element * level.num_steps);
    Transform tfm;
    transform_motion_array_interpolate(&tfm, motion, level.num_steps, time);
    return tfm;
  }
#  else
  (void)time;
#  endif
  return kernel_data_fetch(instance_transforms, level.transform_offset + element);
}

/* Object to world at a time: each level's element transform, outer first, then the object's own (its
 * prototype's). */
ccl_device_inline Transform object_instance_transform(KernelGlobals kg,
                                                      const int object,
                                                      const uint instance,
                                                      const float time)
{
  const KernelObjectInstance placed = kernel_data_fetch(object_instance, object);
#  ifdef __OBJECT_MOTION__
  Transform tfm = (kernel_data_fetch(object_flag, object) & SD_OBJECT_MOTION) ?
                      object_fetch_transform_motion(kg, object, time) :
                      kernel_data_fetch(objects, object).tfm;
#  else
  Transform tfm = kernel_data_fetch(objects, object).tfm;
#  endif
  for (int l = placed.num_levels - 1; l >= 0; l--) {
    const KernelInstanceLevel level = kernel_data_fetch(instance_levels, placed.levels_offset + l);
    tfm = object_instance_level_transform(kg, level, object_instance_element(level, instance), time) *
          tfm;
  }
  return tfm;
}

/* The innermost level's colour, else the object's. */
ccl_device_inline float3 object_instance_color(KernelGlobals kg, const int object, const uint instance)
{
  const KernelObjectInstance placed = kernel_data_fetch(object_instance, object);
  for (int l = placed.num_levels - 1; l >= 0; l--) {
    const KernelInstanceLevel level = kernel_data_fetch(instance_levels, placed.levels_offset + l);
    if (level.color_offset >= 0) {
      const int i = level.color_offset + 3 * int(object_instance_element(level, instance));
      return make_float3(kernel_data_fetch(instance_colors, i),
                         kernel_data_fetch(instance_colors, i + 1),
                         kernel_data_fetch(instance_colors, i + 2));
    }
  }
  const ccl_global KernelObject *kobject = &kernel_data_fetch(objects, object);
  return make_float3(kobject->color[0], kobject->color[1], kobject->color[2]);
}

/* As the object a placement used to be (InstanceExpansion): hash(geometry name, flattened index). */
ccl_device_inline float object_instance_random_number(KernelGlobals kg,
                                                      const int object,
                                                      const uint instance)
{
  const uint hash = hash_uint2(kernel_data_fetch(object_instance, object).name_hash, instance);
  return (float)hash * (1.0f / (float)0xFFFFFFFF);
}
#endif

/* Get transform matrix for shading point. */

ccl_device_inline Transform object_get_transform(KernelGlobals kg,
                                                 const ccl_private ShaderData *sd)
{
#ifdef __OBJECT_MOTION__
  return (sd->object_flag & (SD_OBJECT_MOTION | SD_OBJECT_NESTED)) ?
             sd->ob_tfm_motion :
             object_fetch_transform(kg, sd->object, OBJECT_TRANSFORM);
#else
  return object_fetch_transform(kg, sd->object, OBJECT_TRANSFORM);
#endif
}

ccl_device_inline Transform object_get_inverse_transform(KernelGlobals kg,
                                                         const ccl_private ShaderData *sd)
{
#ifdef __OBJECT_MOTION__
  return (sd->object_flag & (SD_OBJECT_MOTION | SD_OBJECT_NESTED)) ?
             sd->ob_itfm_motion :
             object_fetch_transform(kg, sd->object, OBJECT_INVERSE_TRANSFORM);
#else
  return object_fetch_transform(kg, sd->object, OBJECT_INVERSE_TRANSFORM);
#endif
}

ccl_device_inline Transform lamp_get_inverse_transform(KernelGlobals kg,
                                                       const ccl_global KernelLight *klight)
{
  return object_fetch_transform(kg, klight->object_id, OBJECT_INVERSE_TRANSFORM);
}

/* Transform position from object to world space */

template<class T>
ccl_device_inline void object_position_transform(KernelGlobals kg,
                                                 const ccl_private ShaderData *sd,
                                                 ccl_private T *P)
{
#ifdef __OBJECT_MOTION__
  if (sd->object_flag & (SD_OBJECT_MOTION | SD_OBJECT_NESTED)) {
    *P = transform_point_auto(&sd->ob_tfm_motion, *P);
    return;
  }
#endif

  const Transform tfm = object_fetch_transform(kg, sd->object, OBJECT_TRANSFORM);
  *P = transform_point(&tfm, *P);
}

/* Transform position from world to object space */

template<class T>
ccl_device_inline void object_inverse_position_transform(KernelGlobals kg,
                                                         const ccl_private ShaderData *sd,
                                                         ccl_private T *P)
{
#ifdef __OBJECT_MOTION__
  if (sd->object_flag & (SD_OBJECT_MOTION | SD_OBJECT_NESTED)) {
    *P = transform_point_auto(&sd->ob_itfm_motion, *P);
    return;
  }
#endif

  const Transform tfm = object_fetch_transform(kg, sd->object, OBJECT_INVERSE_TRANSFORM);
  *P = transform_point(&tfm, *P);
}

/* Convenience wrapper that checks for OBJECT_NONE before transforming.
 * Works with both plain types (float3) and dual types (dual3). */
template<class Float3Type>
ccl_device_inline void object_inverse_position_transform_if_object(
    KernelGlobals kg, const ccl_private ShaderData *sd, ccl_private Float3Type *P)
{
  if (sd->object != OBJECT_NONE) {
    object_inverse_position_transform(kg, sd, P);
  }
}

/* Transform normal from world to object space */

ccl_device_inline void object_inverse_normal_transform(KernelGlobals kg,
                                                       const ccl_private ShaderData *sd,
                                                       ccl_private float3 *N)
{
#ifdef __OBJECT_MOTION__
  if (sd->object_flag & (SD_OBJECT_MOTION | SD_OBJECT_NESTED)) {
    if (sd->object != OBJECT_NONE) {
      *N = safe_normalize(transform_direction_transposed_auto(&sd->ob_tfm_motion, *N));
    }
    return;
  }
#endif

  if (sd->object != OBJECT_NONE) {
    const Transform tfm = object_fetch_transform(kg, sd->object, OBJECT_TRANSFORM);
    *N = safe_normalize(transform_direction_transposed(&tfm, *N));
  }
}

/* Transform normal from object to world space */
template<class T>
ccl_device_inline void object_normal_transform(KernelGlobals kg,
                                               const ccl_private ShaderData *sd,
                                               ccl_private T *N)
{
#ifdef __OBJECT_MOTION__
  if (sd->object_flag & (SD_OBJECT_MOTION | SD_OBJECT_NESTED)) {
    *N = normalize(transform_direction_transposed_auto(&sd->ob_itfm_motion, *N));
    return;
  }
#endif

  if (sd->object != OBJECT_NONE) {
    const Transform tfm = object_fetch_transform(kg, sd->object, OBJECT_INVERSE_TRANSFORM);
    *N = normalize(transform_direction_transposed(&tfm, *N));
  }
}

ccl_device_inline bool object_negative_scale_applied(const uint object_flag)
{
  return ((object_flag & SD_OBJECT_NEGATIVE_SCALE) && (object_flag & SD_OBJECT_TRANSFORM_APPLIED));
}

/* Transform direction vector from object to world space */

ccl_device_inline void object_dir_transform(KernelGlobals kg,
                                            const ccl_private ShaderData *sd,
                                            ccl_private float3 *D)
{
#ifdef __OBJECT_MOTION__
  if (sd->object_flag & (SD_OBJECT_MOTION | SD_OBJECT_NESTED)) {
    *D = transform_direction_auto(&sd->ob_tfm_motion, *D);
    return;
  }
#endif

  const Transform tfm = object_fetch_transform(kg, sd->object, OBJECT_TRANSFORM);
  *D = transform_direction(&tfm, *D);
}

/* Transform direction vector from world to object space */

ccl_device_inline void object_inverse_dir_transform(KernelGlobals kg,
                                                    const ccl_private ShaderData *sd,
                                                    ccl_private float3 *D)
{
#ifdef __OBJECT_MOTION__
  if (sd->object_flag & (SD_OBJECT_MOTION | SD_OBJECT_NESTED)) {
    *D = transform_direction_auto(&sd->ob_itfm_motion, *D);
    return;
  }
#endif

  const Transform tfm = object_fetch_transform(kg, sd->object, OBJECT_INVERSE_TRANSFORM);
  *D = transform_direction(&tfm, *D);
}

/* Object center position */

ccl_device_inline float3 object_location(KernelGlobals kg, const ccl_private ShaderData *sd)
{
  if (sd->object == OBJECT_NONE) {
    return make_float3(0.0f, 0.0f, 0.0f);
  }

#ifdef __OBJECT_MOTION__
  if (sd->object_flag & (SD_OBJECT_MOTION | SD_OBJECT_NESTED)) {
    return make_float3(sd->ob_tfm_motion.x.w, sd->ob_tfm_motion.y.w, sd->ob_tfm_motion.z.w);
  }
#endif

  const Transform tfm = object_fetch_transform(kg, sd->object, OBJECT_TRANSFORM);
  return make_float3(tfm.x.w, tfm.y.w, tfm.z.w);
}

/* Color of the object */

ccl_device_inline float3 object_color(KernelGlobals kg, const int object)
{
  if (object == OBJECT_NONE) {
    return make_float3(0.0f, 0.0f, 0.0f);
  }

  const ccl_global KernelObject *kobject = &kernel_data_fetch(objects, object);
  return make_float3(kobject->color[0], kobject->color[1], kobject->color[2]);
}

/* Alpha of the object */

ccl_device_inline float object_alpha(KernelGlobals kg, const int object)
{
  if (object == OBJECT_NONE) {
    return 0.0f;
  }

  return kernel_data_fetch(objects, object).alpha;
}

/* Pass ID number of object */

ccl_device_inline float object_pass_id(KernelGlobals kg, const int object)
{
  if (object == OBJECT_NONE) {
    return 0.0f;
  }

  return kernel_data_fetch(objects, object).pass_id;
}

/* Light-group of object. */

ccl_device_inline int object_lightgroup(KernelGlobals kg, const int object)
{
  if (object == OBJECT_NONE) {
    return LIGHTGROUP_NONE;
  }

  return kernel_data_fetch(objects, object).lightgroup;
}

/* Per object random number for shader variation */

ccl_device_inline float object_random_number(KernelGlobals kg, const int object)
{
  if (object == OBJECT_NONE) {
    return 0.0f;
  }

  return kernel_data_fetch(objects, object).random_number;
}

/* The same for a shading point: a nested object's placement has its own (USD Nodes ADR 0082 › E2). */
ccl_device_inline float3 object_color(KernelGlobals kg, const ccl_private ShaderData *sd)
{
#ifdef __INSTANCE_NESTING__
  if (sd->object_flag & SD_OBJECT_NESTED) {
    return object_instance_color(kg, sd->object, sd->instance);
  }
#endif
  return object_color(kg, sd->object);
}

ccl_device_inline float object_random_number(KernelGlobals kg, const ccl_private ShaderData *sd)
{
#ifdef __INSTANCE_NESTING__
  if (sd->object_flag & SD_OBJECT_NESTED) {
    return object_instance_random_number(kg, sd->object, sd->instance);
  }
#endif
  return object_random_number(kg, sd->object);
}

/* Particle ID from which this object was generated */

ccl_device_inline int object_particle_id(KernelGlobals kg, const int object)
{
  if (object == OBJECT_NONE) {
    return 0;
  }

  return kernel_data_fetch(objects, object).particle_index;
}

/* Generated texture coordinate on surface from where object was instanced */

ccl_device_inline float3 object_dupli_generated(KernelGlobals kg, const int object)
{
  if (object == OBJECT_NONE) {
    return make_float3(0.0f, 0.0f, 0.0f);
  }

  const ccl_global KernelObject *kobject = &kernel_data_fetch(objects, object);
  return make_float3(
      kobject->dupli_generated[0], kobject->dupli_generated[1], kobject->dupli_generated[2]);
}

/* UV texture coordinate on surface from where object was instanced */

ccl_device_inline float3 object_dupli_uv(KernelGlobals kg, const int object)
{
  if (object == OBJECT_NONE) {
    return make_float3(0.0f, 0.0f, 0.0f);
  }

  const ccl_global KernelObject *kobject = &kernel_data_fetch(objects, object);
  return make_float3(kobject->dupli_uv[0], kobject->dupli_uv[1], 0.0f);
}

/* Volume density */

ccl_device_inline float object_volume_density(KernelGlobals kg, const int object)
{
  if (object == OBJECT_NONE) {
    return 1.0f;
  }

  if (kernel_data_fetch(objects, object).primitive_type & PRIMITIVE_TRIANGLE) {
    return kernel_data_fetch(objects, object).mesh_volume.volume_density;
  }

  return 1.0f;
}

/* Pass ID for shader */

ccl_device int shader_pass_id(KernelGlobals kg, const ccl_private ShaderData *sd)
{
  return kernel_data_fetch(shaders, (sd->shader & SHADER_MASK)).pass_id;
}

/* Cryptomatte ID */

ccl_device_inline float object_cryptomatte_id(KernelGlobals kg, const int object)
{
  if (object == OBJECT_NONE) {
    return 0.0f;
  }

  return kernel_data_fetch(objects, object).cryptomatte_object;
}

ccl_device_inline float object_cryptomatte_asset_id(KernelGlobals kg, const int object)
{
  if (object == OBJECT_NONE) {
    return 0;
  }

  return kernel_data_fetch(objects, object).cryptomatte_asset;
}

/* Particle data from which object was instanced */

ccl_device_inline uint particle_index(KernelGlobals kg, const int particle)
{
  return kernel_data_fetch(particles, particle).index;
}

ccl_device float particle_age(KernelGlobals kg, const int particle)
{
  return kernel_data_fetch(particles, particle).age;
}

ccl_device float particle_lifetime(KernelGlobals kg, const int particle)
{
  return kernel_data_fetch(particles, particle).lifetime;
}

ccl_device float particle_size(KernelGlobals kg, const int particle)
{
  return kernel_data_fetch(particles, particle).size;
}

ccl_device float4 particle_rotation(KernelGlobals kg, const int particle)
{
  return kernel_data_fetch(particles, particle).rotation;
}

ccl_device float3 particle_location(KernelGlobals kg, const int particle)
{
  return make_float3(kernel_data_fetch(particles, particle).location);
}

ccl_device float3 particle_velocity(KernelGlobals kg, const int particle)
{
  return make_float3(kernel_data_fetch(particles, particle).velocity);
}

ccl_device float3 particle_angular_velocity(KernelGlobals kg, const int particle)
{
  return make_float3(kernel_data_fetch(particles, particle).angular_velocity);
}

/* Object intersection in BVH */

ccl_device_inline float3 bvh_clamp_direction(const float3 dir)
{
  const float ooeps = 8.271806E-25f;
  return make_float3((fabsf(dir.x) > ooeps) ? dir.x : copysignf(ooeps, dir.x),
                     (fabsf(dir.y) > ooeps) ? dir.y : copysignf(ooeps, dir.y),
                     (fabsf(dir.z) > ooeps) ? dir.z : copysignf(ooeps, dir.z));
}

ccl_device_inline float3 bvh_inverse_direction(const float3 dir)
{
  return reciprocal(dir);
}

/* Transform ray into object space to enter static object in BVH */

ccl_device_inline void bvh_instance_push(KernelGlobals kg,
                                         const int object,
                                         const ccl_private Ray *ray,
                                         ccl_private float3 *P,
                                         ccl_private float3 *dir,
                                         ccl_private float3 *idir)
{
  const Transform tfm = object_fetch_transform(kg, object, OBJECT_INVERSE_TRANSFORM);

  *P = transform_point(&tfm, ray->P);

  *dir = bvh_clamp_direction(transform_direction(&tfm, ray->D));
  *idir = bvh_inverse_direction(*dir);
}

#ifdef __OBJECT_MOTION__
/* Transform ray into object space to enter motion blurred object in BVH */

ccl_device_inline void bvh_instance_motion_push(KernelGlobals kg,
                                                const int object,
                                                const ccl_private Ray *ray,
                                                ccl_private float3 *P,
                                                ccl_private float3 *dir,
                                                ccl_private float3 *idir)
{
  Transform tfm;
  object_fetch_transform_motion_test(kg, object, ray->time, &tfm);

  *P = transform_point(&tfm, ray->P);

  *dir = bvh_clamp_direction(transform_direction(&tfm, ray->D));
  *idir = bvh_inverse_direction(*dir);
}

#endif

#ifdef __INSTANCE_NESTING__
/* Transform ray into a nested object's placement (ADR 0082): its levels composed at the ray's time. For local
 * queries (subsurface, bevel, AO), which go straight to the object's geometry. */
ccl_device_inline void bvh_instance_push_nested(KernelGlobals kg,
                                                const int object,
                                                const int instance,
                                                const ccl_private Ray *ray,
                                                ccl_private float3 *P,
                                                ccl_private float3 *dir,
                                                ccl_private float3 *idir)
{
  const Transform tfm = transform_inverse(
      object_instance_transform(kg, object, uint(instance), ray->time));
  *P = transform_point(&tfm, ray->P);
  *dir = bvh_clamp_direction(transform_direction(&tfm, ray->D));
  *idir = bvh_inverse_direction(*dir);
}
#endif

/* Transform ray to exit static object in BVH. */

ccl_device_inline void bvh_instance_pop(const ccl_private Ray *ray,
                                        ccl_private float3 *P,
                                        ccl_private float3 *dir,
                                        ccl_private float3 *idir)
{
  *P = ray->P;
  *dir = bvh_clamp_direction(ray->D);
  *idir = bvh_inverse_direction(*dir);
}

CCL_NAMESPACE_END
