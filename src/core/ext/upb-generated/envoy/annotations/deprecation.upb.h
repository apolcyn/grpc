/* This file was generated by upbc (the upb compiler) from the input
 * file:
 *
 *     envoy/annotations/deprecation.proto
 *
 * Do not edit -- your changes will be discarded when the file is
 * regenerated. */

#ifndef ENVOY_ANNOTATIONS_DEPRECATION_PROTO_UPB_H_
#define ENVOY_ANNOTATIONS_DEPRECATION_PROTO_UPB_H_

#include "upb/generated_code_support.h"
// Must be last. 
#include "upb/port/def.inc"

#ifdef __cplusplus
extern "C" {
#endif

extern const upb_MiniTableExtension envoy_annotations_disallowed_by_default_ext;
extern const upb_MiniTableExtension envoy_annotations_deprecated_at_minor_version_ext;
extern const upb_MiniTableExtension envoy_annotations_disallowed_by_default_enum_ext;
extern const upb_MiniTableExtension envoy_annotations_deprecated_at_minor_version_enum_ext;
struct google_protobuf_EnumValueOptions;
struct google_protobuf_FieldOptions;
extern const upb_MiniTable google_protobuf_EnumValueOptions_msg_init;
extern const upb_MiniTable google_protobuf_FieldOptions_msg_init;


UPB_INLINE bool envoy_annotations_has_disallowed_by_default(const struct google_protobuf_FieldOptions* msg) {
  return _upb_Message_HasExtensionField(msg, &envoy_annotations_disallowed_by_default_ext);
}
UPB_INLINE void envoy_annotations_clear_disallowed_by_default(struct google_protobuf_FieldOptions* msg) {
  _upb_Message_ClearExtensionField(msg, &envoy_annotations_disallowed_by_default_ext);
}
UPB_INLINE bool envoy_annotations_disallowed_by_default(const struct google_protobuf_FieldOptions* msg) {
  const upb_MiniTableExtension* ext = &envoy_annotations_disallowed_by_default_ext;
  UPB_ASSUME(!upb_IsRepeatedOrMap(&ext->field));
  UPB_ASSUME(_upb_MiniTableField_GetRep(&ext->field) == kUpb_FieldRep_1Byte);
  bool default_val = false;
  bool ret;
  _upb_Message_GetExtensionField(msg, ext, &default_val, &ret);
  return ret;
}
UPB_INLINE void envoy_annotations_set_disallowed_by_default(struct google_protobuf_FieldOptions* msg, bool val, upb_Arena* arena) {
  const upb_MiniTableExtension* ext = &envoy_annotations_disallowed_by_default_ext;
  UPB_ASSUME(!upb_IsRepeatedOrMap(&ext->field));
  UPB_ASSUME(_upb_MiniTableField_GetRep(&ext->field) == kUpb_FieldRep_1Byte);
  bool ok = _upb_Message_SetExtensionField(msg, ext, &val, arena);
  UPB_ASSERT(ok);
}
UPB_INLINE bool envoy_annotations_has_deprecated_at_minor_version(const struct google_protobuf_FieldOptions* msg) {
  return _upb_Message_HasExtensionField(msg, &envoy_annotations_deprecated_at_minor_version_ext);
}
UPB_INLINE void envoy_annotations_clear_deprecated_at_minor_version(struct google_protobuf_FieldOptions* msg) {
  _upb_Message_ClearExtensionField(msg, &envoy_annotations_deprecated_at_minor_version_ext);
}
UPB_INLINE upb_StringView envoy_annotations_deprecated_at_minor_version(const struct google_protobuf_FieldOptions* msg) {
  const upb_MiniTableExtension* ext = &envoy_annotations_deprecated_at_minor_version_ext;
  UPB_ASSUME(!upb_IsRepeatedOrMap(&ext->field));
  UPB_ASSUME(_upb_MiniTableField_GetRep(&ext->field) == kUpb_FieldRep_StringView);
  upb_StringView default_val = upb_StringView_FromString("");
  upb_StringView ret;
  _upb_Message_GetExtensionField(msg, ext, &default_val, &ret);
  return ret;
}
UPB_INLINE void envoy_annotations_set_deprecated_at_minor_version(struct google_protobuf_FieldOptions* msg, upb_StringView val, upb_Arena* arena) {
  const upb_MiniTableExtension* ext = &envoy_annotations_deprecated_at_minor_version_ext;
  UPB_ASSUME(!upb_IsRepeatedOrMap(&ext->field));
  UPB_ASSUME(_upb_MiniTableField_GetRep(&ext->field) == kUpb_FieldRep_StringView);
  bool ok = _upb_Message_SetExtensionField(msg, ext, &val, arena);
  UPB_ASSERT(ok);
}
UPB_INLINE bool envoy_annotations_has_disallowed_by_default_enum(const struct google_protobuf_EnumValueOptions* msg) {
  return _upb_Message_HasExtensionField(msg, &envoy_annotations_disallowed_by_default_enum_ext);
}
UPB_INLINE void envoy_annotations_clear_disallowed_by_default_enum(struct google_protobuf_EnumValueOptions* msg) {
  _upb_Message_ClearExtensionField(msg, &envoy_annotations_disallowed_by_default_enum_ext);
}
UPB_INLINE bool envoy_annotations_disallowed_by_default_enum(const struct google_protobuf_EnumValueOptions* msg) {
  const upb_MiniTableExtension* ext = &envoy_annotations_disallowed_by_default_enum_ext;
  UPB_ASSUME(!upb_IsRepeatedOrMap(&ext->field));
  UPB_ASSUME(_upb_MiniTableField_GetRep(&ext->field) == kUpb_FieldRep_1Byte);
  bool default_val = false;
  bool ret;
  _upb_Message_GetExtensionField(msg, ext, &default_val, &ret);
  return ret;
}
UPB_INLINE void envoy_annotations_set_disallowed_by_default_enum(struct google_protobuf_EnumValueOptions* msg, bool val, upb_Arena* arena) {
  const upb_MiniTableExtension* ext = &envoy_annotations_disallowed_by_default_enum_ext;
  UPB_ASSUME(!upb_IsRepeatedOrMap(&ext->field));
  UPB_ASSUME(_upb_MiniTableField_GetRep(&ext->field) == kUpb_FieldRep_1Byte);
  bool ok = _upb_Message_SetExtensionField(msg, ext, &val, arena);
  UPB_ASSERT(ok);
}
UPB_INLINE bool envoy_annotations_has_deprecated_at_minor_version_enum(const struct google_protobuf_EnumValueOptions* msg) {
  return _upb_Message_HasExtensionField(msg, &envoy_annotations_deprecated_at_minor_version_enum_ext);
}
UPB_INLINE void envoy_annotations_clear_deprecated_at_minor_version_enum(struct google_protobuf_EnumValueOptions* msg) {
  _upb_Message_ClearExtensionField(msg, &envoy_annotations_deprecated_at_minor_version_enum_ext);
}
UPB_INLINE upb_StringView envoy_annotations_deprecated_at_minor_version_enum(const struct google_protobuf_EnumValueOptions* msg) {
  const upb_MiniTableExtension* ext = &envoy_annotations_deprecated_at_minor_version_enum_ext;
  UPB_ASSUME(!upb_IsRepeatedOrMap(&ext->field));
  UPB_ASSUME(_upb_MiniTableField_GetRep(&ext->field) == kUpb_FieldRep_StringView);
  upb_StringView default_val = upb_StringView_FromString("");
  upb_StringView ret;
  _upb_Message_GetExtensionField(msg, ext, &default_val, &ret);
  return ret;
}
UPB_INLINE void envoy_annotations_set_deprecated_at_minor_version_enum(struct google_protobuf_EnumValueOptions* msg, upb_StringView val, upb_Arena* arena) {
  const upb_MiniTableExtension* ext = &envoy_annotations_deprecated_at_minor_version_enum_ext;
  UPB_ASSUME(!upb_IsRepeatedOrMap(&ext->field));
  UPB_ASSUME(_upb_MiniTableField_GetRep(&ext->field) == kUpb_FieldRep_StringView);
  bool ok = _upb_Message_SetExtensionField(msg, ext, &val, arena);
  UPB_ASSERT(ok);
}
extern const upb_MiniTableFile envoy_annotations_deprecation_proto_upb_file_layout;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#include "upb/port/undef.inc"

#endif  /* ENVOY_ANNOTATIONS_DEPRECATION_PROTO_UPB_H_ */
