/* A minimal in-process JNI environment. */

#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "error.h"
#include "jni_env.h"

/* ------------------------------------------------------------- object model */

typedef enum {
  OBJ_STRING,
  OBJ_ARRAY,
  OBJ_BITMAP,
  OBJ_CLASS,
  OBJ_CONTEXT,
} ObjKind;

/* Stamped into every object this layer hands out. The guest can pass a
 * reference back through any JNI entry point, so each one checks the stamp
 * before dereferencing rather than trusting the pointer. */
#define JOBJ_MAGIC 0x424F424Au /* "BOBJ" */

typedef struct JObj {
  uint32_t magic;
  ObjKind kind;
  int refs;
  union {
    struct {
      char *utf8;
      int utf8_length;
      jchar *utf16;
      int utf16_length;
    } string;
    struct {
      char type; /* 'Z','B','C','S','I','J','F','D','L' */
      int element_size;
      int length;
      void *data;
    } array;
    JBitmap bitmap;
    const char *class_name;
  };
} JObj;

static JObj *obj_new(ObjKind kind) {
  JObj *object = calloc(1, sizeof *object);
  if (!object) fatal_error("Out of memory allocating a JNI object.");
  object->magic = JOBJ_MAGIC;
  object->kind = kind;
  object->refs = 1;
  return object;
}

/* NULL unless the reference really is one of ours. */
static JObj *obj_of(jobject handle) {
  JObj *object = (JObj *)handle;
  return (object && object->magic == JOBJ_MAGIC) ? object : NULL;
}

static void obj_free(JObj *object) {
  if (!object) return;
  object->magic = 0;
  switch (object->kind) {
    case OBJ_STRING:
      free(object->string.utf8);
      free(object->string.utf16);
      break;
    case OBJ_ARRAY:
      free(object->array.data);
      break;
    case OBJ_BITMAP:
      free(object->bitmap.pixels);
      break;
    case OBJ_CLASS:
    case OBJ_CONTEXT:
      break;
  }
  free(object);
}

/* ----------------------------------------------------------------- classes */

/* Classes are interned by name; the guest only ever compares and passes them
 * back to us, so a stable pointer per name is all that is needed. */
#define CLASS_MAX 32
static JObj *g_classes[CLASS_MAX];
static const char *g_class_names[CLASS_MAX];
static int g_class_count;
static Mutex g_class_lock;

static jclass intern_class(const char *name) {
  if (!name) name = "?";
  mutexLock(&g_class_lock);
  for (int i = 0; i < g_class_count; i++) {
    if (!strcmp(g_class_names[i], name)) {
      mutexUnlock(&g_class_lock);
      return (jclass)g_classes[i];
    }
  }
  if (g_class_count >= CLASS_MAX) {
    mutexUnlock(&g_class_lock);
    return (jclass)g_classes[0];
  }
  JObj *object = obj_new(OBJ_CLASS);
  object->class_name = strdup(name);
  g_class_names[g_class_count] = object->class_name;
  g_classes[g_class_count] = object;
  g_class_count++;
  mutexUnlock(&g_class_lock);
  return (jclass)object;
}

/* --------------------------------------------------------------- context */

/* Stands in for the Java BallisticaContext instance. The engine holds it
 * across threads with NewGlobalRef, so it has to be a real object rather
 * than a placeholder address. */
static JObj *g_context;

jobject jni_context_object(void) { return (jobject)g_context; }

/* ---------------------------------------------------------------- strings */

jstring jni_new_string(const char *utf8) {
  if (!utf8) return NULL;
  JObj *object = obj_new(OBJ_STRING);
  object->string.utf8_length = (int)strlen(utf8);
  object->string.utf8 = malloc(object->string.utf8_length + 1);
  if (!object->string.utf8) fatal_error("Out of memory allocating a JNI string.");
  memcpy(object->string.utf8, utf8, object->string.utf8_length + 1);
  return (jstring)object;
}

const char *jni_string_utf8(jstring string) {
  JObj *object = obj_of((jobject)string);
  if (!object || object->kind != OBJ_STRING) return NULL;
  return object->string.utf8;
}

/* Decode enough UTF-8 to serve GetStringChars/GetStringLength. Surrogate
 * pairs are emitted for astral code points, matching Java's UTF-16. */
static void string_build_utf16(JObj *object) {
  if (object->string.utf16) return;
  const unsigned char *cursor = (const unsigned char *)object->string.utf8;
  const int capacity = object->string.utf8_length * 2 + 2;
  jchar *out = malloc((size_t)capacity * sizeof(jchar));
  if (!out) fatal_error("Out of memory converting a JNI string.");

  int count = 0;
  while (*cursor) {
    unsigned int code;
    if (*cursor < 0x80) {
      code = *cursor++;
    } else if ((*cursor & 0xE0) == 0xC0) {
      code = (*cursor++ & 0x1Fu) << 6;
      code |= (*cursor ? (*cursor++ & 0x3Fu) : 0);
    } else if ((*cursor & 0xF0) == 0xE0) {
      code = (*cursor++ & 0x0Fu) << 12;
      code |= (*cursor ? (*cursor++ & 0x3Fu) << 6 : 0);
      code |= (*cursor ? (*cursor++ & 0x3Fu) : 0);
    } else {
      code = (*cursor++ & 0x07u) << 18;
      code |= (*cursor ? (*cursor++ & 0x3Fu) << 12 : 0);
      code |= (*cursor ? (*cursor++ & 0x3Fu) << 6 : 0);
      code |= (*cursor ? (*cursor++ & 0x3Fu) : 0);
    }
    if (code >= 0x10000u) {
      code -= 0x10000u;
      out[count++] = (jchar)(0xD800u + (code >> 10));
      out[count++] = (jchar)(0xDC00u + (code & 0x3FFu));
    } else {
      out[count++] = (jchar)code;
    }
  }
  out[count] = 0;
  object->string.utf16 = out;
  object->string.utf16_length = count;
}

/* ----------------------------------------------------------------- arrays */

static int element_size_for(char type) {
  switch (type) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'I': case 'F': return 4;
    case 'J': case 'D': case 'L': return 8;
    default: return 8;
  }
}

static jarray array_new(char type, int length) {
  if (length < 0) return NULL;
  JObj *object = obj_new(OBJ_ARRAY);
  object->array.type = type;
  object->array.element_size = element_size_for(type);
  object->array.length = length;
  object->array.data = calloc((size_t)length ? (size_t)length : 1,
                              (size_t)object->array.element_size);
  if (!object->array.data) fatal_error("Out of memory allocating a JNI array.");
  return (jarray)object;
}

static JObj *array_of(jarray array) {
  JObj *object = obj_of((jobject)array);
  return (object && object->kind == OBJ_ARRAY) ? object : NULL;
}

jobject jni_new_float_array(const float *values, int count) {
  jarray array = array_new('F', count);
  JObj *object = array_of(array);
  if (object && values) memcpy(object->array.data, values, (size_t)count * 4);
  return (jobject)array;
}

const float *jni_float_array_data(jobject array) {
  JObj *object = array_of((jarray)array);
  if (!object || object->array.type != 'F') return NULL;
  return (const float *)object->array.data;
}

int jni_array_length(jobject array) {
  JObj *object = array_of((jarray)array);
  return object ? object->array.length : 0;
}

const char *jni_object_array_utf8(jobject array, int index) {
  JObj *object = array_of((jarray)array);
  if (!object || object->array.type != 'L') return NULL;
  if (index < 0 || index >= object->array.length) return NULL;
  jobject *slots = object->array.data;
  return jni_string_utf8((jstring)slots[index]);
}

/* ---------------------------------------------------------------- bitmaps */

jobject jni_new_bitmap(int width, int height) {
  if (width <= 0) width = 1;
  if (height <= 0) height = 1;
  JObj *object = obj_new(OBJ_BITMAP);
  object->bitmap.width = width;
  object->bitmap.height = height;
  object->bitmap.stride = width * 4;
  object->bitmap.pixels = calloc((size_t)width * (size_t)height, 4);
  if (!object->bitmap.pixels) fatal_error("Out of memory allocating a bitmap.");
  return (jobject)object;
}

JBitmap *jni_bitmap(jobject handle) {
  JObj *object = obj_of(handle);
  return (object && object->kind == OBJ_BITMAP) ? &object->bitmap : NULL;
}

/* ------------------------------------------------------------ arg cursors */

jboolean jargs_boolean(JArgs *args) {
  if (args->from_array) return args->values[args->index++].z;
  return (jboolean)(va_arg(*args->va, int) != 0);
}
jint jargs_int(JArgs *args) {
  if (args->from_array) return args->values[args->index++].i;
  return va_arg(*args->va, jint);
}
jlong jargs_long(JArgs *args) {
  if (args->from_array) return args->values[args->index++].j;
  return va_arg(*args->va, jlong);
}
jfloat jargs_float(JArgs *args) {
  if (args->from_array) return args->values[args->index++].f;
  /* Varargs promote float to double. */
  return (jfloat)va_arg(*args->va, double);
}
jdouble jargs_double(JArgs *args) {
  if (args->from_array) return args->values[args->index++].d;
  return va_arg(*args->va, double);
}
jobject jargs_object(JArgs *args) {
  if (args->from_array) return args->values[args->index++].l;
  return va_arg(*args->va, jobject);
}

/* -------------------------------------------------------- member lookups */

static int lookup_member(const JMemberDesc *table, int count, const char *name) {
  if (!name) return 0;
  for (int i = 0; i < count; i++)
    if (!strcmp(table[i].name, name)) return table[i].id;
  return 0;
}

static const JMemberDesc *describe_member(const JMemberDesc *table, int count, int id) {
  for (int i = 0; i < count; i++)
    if (table[i].id == id) return &table[i];
  return NULL;
}

/* --------------------------------------------------------------- JNI body */

#define UNUSED_ENV (void)env

static jint jni_GetVersion(JNIEnv *env) { UNUSED_ENV; return JNI_VERSION_1_6; }

static jclass jni_FindClass(JNIEnv *env, const char *name) {
  UNUSED_ENV;
  return intern_class(name);
}

static jclass jni_GetObjectClass(JNIEnv *env, jobject object) {
  UNUSED_ENV;
  JObj *o = obj_of(object);
  if (o && o->kind == OBJ_STRING) return intern_class("java/lang/String");
  if (o && o->kind == OBJ_BITMAP) return intern_class("android/graphics/Bitmap");
  return intern_class("com/ericfroemling/ballistica/BallisticaContext");
}

static jclass jni_GetSuperclass(JNIEnv *env, jclass c) {
  UNUSED_ENV; (void)c;
  return intern_class("java/lang/Object");
}
static jboolean jni_IsAssignableFrom(JNIEnv *env, jclass a, jclass b) {
  UNUSED_ENV; (void)a; (void)b;
  return JNI_TRUE;
}
static jboolean jni_IsInstanceOf(JNIEnv *env, jobject o, jclass c) {
  UNUSED_ENV; (void)o; (void)c;
  return JNI_TRUE;
}
static jboolean jni_IsSameObject(JNIEnv *env, jobject a, jobject b) {
  UNUSED_ENV;
  return a == b ? JNI_TRUE : JNI_FALSE;
}

static jmethodID jni_GetMethodID(JNIEnv *env, jclass c, const char *name,
                                 const char *sig) {
  UNUSED_ENV; (void)c; (void)sig;
  const int id = lookup_member(java_methods, java_method_count, name);
  if (!id) trace("JNI: unmapped method %s %s", name ? name : "?", sig ? sig : "");
  return (jmethodID)(intptr_t)id;
}

static jfieldID jni_GetFieldID(JNIEnv *env, jclass c, const char *name,
                               const char *sig) {
  UNUSED_ENV; (void)c; (void)sig;
  const int id = lookup_member(java_fields, java_field_count, name);
  if (!id) trace("JNI: unmapped field %s %s", name ? name : "?", sig ? sig : "");
  return (jfieldID)(intptr_t)id;
}

/* Method dispatch. Every Call*Method flavour funnels into these. */
#define CALL_BODY(Kind, Type, Dispatch, Zero)                                  \
  static Type jni_Call##Kind##MethodV(JNIEnv *env, jobject obj,                \
                                      jmethodID method, va_list va) {          \
    UNUSED_ENV; (void)obj;                                                     \
    JArgs args = {0, &va, NULL, 0};                                            \
    return Dispatch((int)(intptr_t)method, &args);                             \
  }                                                                            \
  static Type jni_Call##Kind##MethodA(JNIEnv *env, jobject obj,                \
                                      jmethodID method, const jvalue *values) {\
    UNUSED_ENV; (void)obj;                                                     \
    JArgs args = {1, NULL, values, 0};                                         \
    return Dispatch((int)(intptr_t)method, &args);                             \
  }                                                                            \
  static Type jni_Call##Kind##Method(JNIEnv *env, jobject obj,                 \
                                     jmethodID method, ...) {                  \
    va_list va;                                                                \
    va_start(va, method);                                                      \
    JArgs args = {0, &va, NULL, 0};                                            \
    Type result = Dispatch((int)(intptr_t)method, &args);                      \
    va_end(va);                                                                \
    UNUSED_ENV; (void)obj;                                                     \
    return result;                                                             \
  }                                                                            \
  static Type jni_CallNonvirtual##Kind##MethodV(                               \
      JNIEnv *env, jobject obj, jclass c, jmethodID method, va_list va) {      \
    UNUSED_ENV; (void)obj; (void)c;                                            \
    JArgs args = {0, &va, NULL, 0};                                            \
    return Dispatch((int)(intptr_t)method, &args);                             \
  }                                                                            \
  static Type jni_CallNonvirtual##Kind##MethodA(                               \
      JNIEnv *env, jobject obj, jclass c, jmethodID method,                    \
      const jvalue *values) {                                                  \
    UNUSED_ENV; (void)obj; (void)c;                                            \
    JArgs args = {1, NULL, values, 0};                                         \
    return Dispatch((int)(intptr_t)method, &args);                             \
  }                                                                            \
  static Type jni_CallNonvirtual##Kind##Method(JNIEnv *env, jobject obj,       \
                                               jclass c, jmethodID method,     \
                                               ...) {                          \
    va_list va;                                                                \
    va_start(va, method);                                                      \
    JArgs args = {0, &va, NULL, 0};                                            \
    Type result = Dispatch((int)(intptr_t)method, &args);                      \
    va_end(va);                                                                \
    UNUSED_ENV; (void)obj; (void)c;                                            \
    return result;                                                             \
  }                                                                            \
  static Type jni_CallStatic##Kind##MethodV(JNIEnv *env, jclass c,             \
                                            jmethodID method, va_list va) {    \
    UNUSED_ENV; (void)c;                                                       \
    JArgs args = {0, &va, NULL, 0};                                            \
    return Dispatch((int)(intptr_t)method, &args);                             \
  }                                                                            \
  static Type jni_CallStatic##Kind##MethodA(JNIEnv *env, jclass c,             \
                                            jmethodID method,                  \
                                            const jvalue *values) {            \
    UNUSED_ENV; (void)c;                                                       \
    JArgs args = {1, NULL, values, 0};                                         \
    return Dispatch((int)(intptr_t)method, &args);                             \
  }                                                                            \
  static Type jni_CallStatic##Kind##Method(JNIEnv *env, jclass c,              \
                                           jmethodID method, ...) {            \
    va_list va;                                                                \
    va_start(va, method);                                                      \
    JArgs args = {0, &va, NULL, 0};                                            \
    Type result = Dispatch((int)(intptr_t)method, &args);                      \
    va_end(va);                                                                \
    UNUSED_ENV; (void)c;                                                       \
    return result;                                                             \
  }

/* void needs its own expansion because it cannot name a result. */
static void jni_CallVoidMethodV(JNIEnv *env, jobject obj, jmethodID method, va_list va) {
  UNUSED_ENV; (void)obj;
  JArgs args = {0, &va, NULL, 0};
  java_call_void((int)(intptr_t)method, &args);
}
static void jni_CallVoidMethodA(JNIEnv *env, jobject obj, jmethodID method,
                                const jvalue *values) {
  UNUSED_ENV; (void)obj;
  JArgs args = {1, NULL, values, 0};
  java_call_void((int)(intptr_t)method, &args);
}
static void jni_CallVoidMethod(JNIEnv *env, jobject obj, jmethodID method, ...) {
  va_list va;
  va_start(va, method);
  jni_CallVoidMethodV(env, obj, method, va);
  va_end(va);
}
static void jni_CallNonvirtualVoidMethodV(JNIEnv *env, jobject obj, jclass c,
                                          jmethodID method, va_list va) {
  (void)c;
  jni_CallVoidMethodV(env, obj, method, va);
}
static void jni_CallNonvirtualVoidMethodA(JNIEnv *env, jobject obj, jclass c,
                                          jmethodID method, const jvalue *values) {
  (void)c;
  jni_CallVoidMethodA(env, obj, method, values);
}
static void jni_CallNonvirtualVoidMethod(JNIEnv *env, jobject obj, jclass c,
                                         jmethodID method, ...) {
  va_list va;
  va_start(va, method);
  (void)c;
  jni_CallVoidMethodV(env, obj, method, va);
  va_end(va);
}
static void jni_CallStaticVoidMethodV(JNIEnv *env, jclass c, jmethodID method,
                                      va_list va) {
  (void)c;
  jni_CallVoidMethodV(env, NULL, method, va);
}
static void jni_CallStaticVoidMethodA(JNIEnv *env, jclass c, jmethodID method,
                                      const jvalue *values) {
  (void)c;
  jni_CallVoidMethodA(env, NULL, method, values);
}
static void jni_CallStaticVoidMethod(JNIEnv *env, jclass c, jmethodID method, ...) {
  va_list va;
  va_start(va, method);
  (void)c;
  jni_CallVoidMethodV(env, NULL, method, va);
  va_end(va);
}

static jbyte dispatch_byte(int id, JArgs *args) { return (jbyte)java_call_int(id, args); }
static jchar dispatch_char(int id, JArgs *args) { return (jchar)java_call_int(id, args); }
static jshort dispatch_short(int id, JArgs *args) { return (jshort)java_call_int(id, args); }

CALL_BODY(Object, jobject, java_call_object, NULL)
CALL_BODY(Boolean, jboolean, java_call_boolean, JNI_FALSE)
CALL_BODY(Byte, jbyte, dispatch_byte, 0)
CALL_BODY(Char, jchar, dispatch_char, 0)
CALL_BODY(Short, jshort, dispatch_short, 0)
CALL_BODY(Int, jint, java_call_int, 0)
CALL_BODY(Long, jlong, java_call_long, 0)
CALL_BODY(Float, jfloat, java_call_float, 0.0f)
CALL_BODY(Double, jdouble, java_call_double, 0.0)

/* Field access. */
static jobject jni_GetObjectField(JNIEnv *env, jobject obj, jfieldID field) {
  UNUSED_ENV; (void)obj;
  return java_get_object_field((int)(intptr_t)field);
}
static jint jni_GetIntField(JNIEnv *env, jobject obj, jfieldID field) {
  UNUSED_ENV; (void)obj;
  return java_get_int_field((int)(intptr_t)field);
}
static jobject jni_GetStaticObjectField(JNIEnv *env, jclass c, jfieldID field) {
  UNUSED_ENV; (void)c;
  return java_get_object_field((int)(intptr_t)field);
}
static jint jni_GetStaticIntField(JNIEnv *env, jclass c, jfieldID field) {
  UNUSED_ENV; (void)c;
  return java_get_int_field((int)(intptr_t)field);
}

#define FIELD_ZERO(Kind, Type, Zero)                                          \
  static Type jni_Get##Kind##Field(JNIEnv *env, jobject o, jfieldID f) {      \
    UNUSED_ENV; (void)o; (void)f; return Zero;                                \
  }                                                                           \
  static Type jni_GetStatic##Kind##Field(JNIEnv *env, jclass c, jfieldID f) { \
    UNUSED_ENV; (void)c; (void)f; return Zero;                                \
  }                                                                           \
  static void jni_Set##Kind##Field(JNIEnv *env, jobject o, jfieldID f, Type v) {\
    UNUSED_ENV; (void)o; (void)f; (void)v;                                    \
  }                                                                           \
  static void jni_SetStatic##Kind##Field(JNIEnv *env, jclass c, jfieldID f,   \
                                         Type v) {                            \
    UNUSED_ENV; (void)c; (void)f; (void)v;                                    \
  }

FIELD_ZERO(Boolean, jboolean, JNI_FALSE)
FIELD_ZERO(Byte, jbyte, 0)
FIELD_ZERO(Char, jchar, 0)
FIELD_ZERO(Short, jshort, 0)
FIELD_ZERO(Long, jlong, 0)
FIELD_ZERO(Float, jfloat, 0.0f)
FIELD_ZERO(Double, jdouble, 0.0)

static void jni_SetObjectField(JNIEnv *env, jobject o, jfieldID f, jobject v) {
  UNUSED_ENV; (void)o; (void)f; (void)v;
}
static void jni_SetStaticObjectField(JNIEnv *env, jclass c, jfieldID f, jobject v) {
  UNUSED_ENV; (void)c; (void)f; (void)v;
}
static void jni_SetIntField(JNIEnv *env, jobject o, jfieldID f, jint v) {
  UNUSED_ENV; (void)o; (void)f; (void)v;
}
static void jni_SetStaticIntField(JNIEnv *env, jclass c, jfieldID f, jint v) {
  UNUSED_ENV; (void)c; (void)f; (void)v;
}

static jmethodID jni_GetStaticMethodID(JNIEnv *env, jclass c, const char *name,
                                       const char *sig) {
  return jni_GetMethodID(env, c, name, sig);
}
static jfieldID jni_GetStaticFieldID(JNIEnv *env, jclass c, const char *name,
                                     const char *sig) {
  return jni_GetFieldID(env, c, name, sig);
}

/* Strings. */
static jstring jni_NewStringUTF(JNIEnv *env, const char *utf8) {
  UNUSED_ENV;
  return jni_new_string(utf8);
}
static jsize jni_GetStringUTFLength(JNIEnv *env, jstring s) {
  UNUSED_ENV;
  JObj *o = obj_of((jobject)s);
  return (o && o->kind == OBJ_STRING) ? o->string.utf8_length : 0;
}
static const char *jni_GetStringUTFChars(JNIEnv *env, jstring s, jboolean *copy) {
  UNUSED_ENV;
  if (copy) *copy = JNI_FALSE;
  return jni_string_utf8(s);
}
static void jni_ReleaseStringUTFChars(JNIEnv *env, jstring s, char *chars) {
  UNUSED_ENV; (void)s; (void)chars;
}
static jstring jni_NewString(JNIEnv *env, const jchar *chars, jsize length) {
  UNUSED_ENV;
  /* Encode UTF-16 (including surrogate pairs) back into UTF-8. */
  char *utf8 = malloc((size_t)length * 4 + 1);
  if (!utf8) fatal_error("Out of memory allocating a JNI string.");
  size_t out = 0;
  for (jsize i = 0; i < length; i++) {
    unsigned int code = chars[i];
    if (code >= 0xD800u && code <= 0xDBFFu && i + 1 < length &&
        chars[i + 1] >= 0xDC00u && chars[i + 1] <= 0xDFFFu) {
      code = 0x10000u + ((code - 0xD800u) << 10) + (chars[++i] - 0xDC00u);
    }
    if (code < 0x80u) {
      utf8[out++] = (char)code;
    } else if (code < 0x800u) {
      utf8[out++] = (char)(0xC0u | (code >> 6));
      utf8[out++] = (char)(0x80u | (code & 0x3Fu));
    } else if (code < 0x10000u) {
      utf8[out++] = (char)(0xE0u | (code >> 12));
      utf8[out++] = (char)(0x80u | ((code >> 6) & 0x3Fu));
      utf8[out++] = (char)(0x80u | (code & 0x3Fu));
    } else {
      utf8[out++] = (char)(0xF0u | (code >> 18));
      utf8[out++] = (char)(0x80u | ((code >> 12) & 0x3Fu));
      utf8[out++] = (char)(0x80u | ((code >> 6) & 0x3Fu));
      utf8[out++] = (char)(0x80u | (code & 0x3Fu));
    }
  }
  utf8[out] = '\0';
  jstring result = jni_new_string(utf8);
  free(utf8);
  return result;
}
static jsize jni_GetStringLength(JNIEnv *env, jstring s) {
  UNUSED_ENV;
  JObj *o = obj_of((jobject)s);
  if (!o || o->kind != OBJ_STRING) return 0;
  string_build_utf16(o);
  return o->string.utf16_length;
}
static const jchar *jni_GetStringChars(JNIEnv *env, jstring s, jboolean *copy) {
  UNUSED_ENV;
  if (copy) *copy = JNI_FALSE;
  JObj *o = obj_of((jobject)s);
  if (!o || o->kind != OBJ_STRING) return NULL;
  string_build_utf16(o);
  return o->string.utf16;
}
static void jni_ReleaseStringChars(JNIEnv *env, jstring s, const jchar *chars) {
  UNUSED_ENV; (void)s; (void)chars;
}
static void jni_GetStringRegion(JNIEnv *env, jstring s, jsize start, jsize len,
                                jchar *buf) {
  const jchar *chars = jni_GetStringChars(env, s, NULL);
  if (chars) memcpy(buf, chars + start, (size_t)len * sizeof(jchar));
}
static void jni_GetStringUTFRegion(JNIEnv *env, jstring s, jsize start, jsize len,
                                   char *buf) {
  UNUSED_ENV;
  const char *utf8 = jni_string_utf8(s);
  if (utf8) {
    memcpy(buf, utf8 + start, (size_t)len);
    buf[len] = '\0';
  }
}
static const jchar *jni_GetStringCritical(JNIEnv *env, jstring s, jboolean *copy) {
  return jni_GetStringChars(env, s, copy);
}
static void jni_ReleaseStringCritical(JNIEnv *env, jstring s, const jchar *chars) {
  jni_ReleaseStringChars(env, s, chars);
}

/* Arrays. */
static jsize jni_GetArrayLength(JNIEnv *env, jarray a) {
  UNUSED_ENV;
  JObj *o = array_of(a);
  return o ? o->array.length : 0;
}
static jobjectArray jni_NewObjectArray(JNIEnv *env, jsize length, jclass c,
                                       jobject initial) {
  UNUSED_ENV; (void)c;
  jarray array = array_new('L', length);
  JObj *o = array_of(array);
  if (o && initial) {
    jobject *slots = o->array.data;
    for (jsize i = 0; i < length; i++) slots[i] = initial;
  }
  return (jobjectArray)array;
}
static jobject jni_GetObjectArrayElement(JNIEnv *env, jobjectArray a, jsize index) {
  UNUSED_ENV;
  JObj *o = array_of(a);
  if (!o || index < 0 || index >= o->array.length) return NULL;
  return ((jobject *)o->array.data)[index];
}
static void jni_SetObjectArrayElement(JNIEnv *env, jobjectArray a, jsize index,
                                      jobject value) {
  UNUSED_ENV;
  JObj *o = array_of(a);
  if (!o || index < 0 || index >= o->array.length) return;
  ((jobject *)o->array.data)[index] = value;
}

#define ARRAY_BODY(Kind, Type, Tag)                                            \
  static Type##Array jni_New##Kind##Array(JNIEnv *env, jsize length) {         \
    UNUSED_ENV; return (Type##Array)array_new(Tag, length);                    \
  }                                                                            \
  static Type *jni_Get##Kind##ArrayElements(JNIEnv *env, Type##Array a,        \
                                            jboolean *copy) {                  \
    UNUSED_ENV; if (copy) *copy = JNI_FALSE;                                   \
    JObj *o = array_of(a); return o ? (Type *)o->array.data : NULL;            \
  }                                                                            \
  static void jni_Release##Kind##ArrayElements(JNIEnv *env, Type##Array a,     \
                                               Type *elems, jint mode) {       \
    UNUSED_ENV; (void)a; (void)elems; (void)mode;                              \
  }                                                                            \
  static void jni_Get##Kind##ArrayRegion(JNIEnv *env, Type##Array a,           \
                                         jsize start, jsize len, Type *buf) {  \
    UNUSED_ENV;                                                                \
    JObj *o = array_of(a);                                                     \
    if (o && start >= 0 && start + len <= o->array.length)                     \
      memcpy(buf, (Type *)o->array.data + start, (size_t)len * sizeof(Type));  \
  }                                                                            \
  static void jni_Set##Kind##ArrayRegion(JNIEnv *env, Type##Array a,           \
                                         jsize start, jsize len,               \
                                         const Type *buf) {                    \
    UNUSED_ENV;                                                                \
    JObj *o = array_of(a);                                                     \
    if (o && start >= 0 && start + len <= o->array.length)                     \
      memcpy((Type *)o->array.data + start, buf, (size_t)len * sizeof(Type));  \
  }

ARRAY_BODY(Boolean, jboolean, 'Z')
ARRAY_BODY(Byte, jbyte, 'B')
ARRAY_BODY(Char, jchar, 'C')
ARRAY_BODY(Short, jshort, 'S')
ARRAY_BODY(Int, jint, 'I')
ARRAY_BODY(Long, jlong, 'J')
ARRAY_BODY(Float, jfloat, 'F')
ARRAY_BODY(Double, jdouble, 'D')

static void *jni_GetPrimitiveArrayCritical(JNIEnv *env, jarray a, jboolean *copy) {
  UNUSED_ENV;
  if (copy) *copy = JNI_FALSE;
  JObj *o = array_of(a);
  return o ? o->array.data : NULL;
}
static void jni_ReleasePrimitiveArrayCritical(JNIEnv *env, jarray a, void *data,
                                              jint mode) {
  UNUSED_ENV; (void)a; (void)data; (void)mode;
}

/* References are counted for real. The engine measures text constantly, and
 * each measurement makes a string and a float array; treating the deletes as
 * no-ops would leak both on every frame that changes a label. */
static jobject retain(jobject handle) {
  JObj *object = obj_of(handle);
  if (object) __atomic_add_fetch(&object->refs, 1, __ATOMIC_ACQ_REL);
  return handle;
}

static void release(jobject handle) {
  JObj *object = obj_of(handle);
  if (!object) return;
  /* Interned classes and the context live for the whole process. */
  if (object->kind == OBJ_CLASS || object->kind == OBJ_CONTEXT) return;
  if (__atomic_sub_fetch(&object->refs, 1, __ATOMIC_ACQ_REL) <= 0) obj_free(object);
}

static jobject jni_NewGlobalRef(JNIEnv *env, jobject o) { UNUSED_ENV; return retain(o); }
static void jni_DeleteGlobalRef(JNIEnv *env, jobject o) { UNUSED_ENV; release(o); }
static jobject jni_NewLocalRef(JNIEnv *env, jobject o) { UNUSED_ENV; return retain(o); }
static void jni_DeleteLocalRef(JNIEnv *env, jobject o) { UNUSED_ENV; release(o); }
static jweak jni_NewWeakGlobalRef(JNIEnv *env, jobject o) { UNUSED_ENV; return retain(o); }
static void jni_DeleteWeakGlobalRef(JNIEnv *env, jweak o) { UNUSED_ENV; release(o); }
static jint jni_EnsureLocalCapacity(JNIEnv *env, jint n) { UNUSED_ENV; (void)n; return 0; }
static jint jni_PushLocalFrame(JNIEnv *env, jint n) { UNUSED_ENV; (void)n; return 0; }
static jobject jni_PopLocalFrame(JNIEnv *env, jobject o) { UNUSED_ENV; return o; }

/* Exceptions never originate here. */
static jthrowable jni_ExceptionOccurred(JNIEnv *env) { UNUSED_ENV; return NULL; }
static void jni_ExceptionDescribe(JNIEnv *env) { UNUSED_ENV; }
static void jni_ExceptionClear(JNIEnv *env) { UNUSED_ENV; }
static jboolean jni_ExceptionCheck(JNIEnv *env) { UNUSED_ENV; return JNI_FALSE; }
static jint jni_Throw(JNIEnv *env, jthrowable t) { UNUSED_ENV; (void)t; return 0; }
static jint jni_ThrowNew(JNIEnv *env, jclass c, const char *message) {
  UNUSED_ENV; (void)c;
  trace("JNI: guest threw %s", message ? message : "");
  return 0;
}
static void jni_FatalError(JNIEnv *env, const char *message) {
  UNUSED_ENV;
  fatal_error("JNI FatalError: %s", message ? message : "");
}

static jint jni_MonitorEnter(JNIEnv *env, jobject o) { UNUSED_ENV; (void)o; return 0; }
static jint jni_MonitorExit(JNIEnv *env, jobject o) { UNUSED_ENV; (void)o; return 0; }
static jint jni_RegisterNatives(JNIEnv *env, jclass c, const JNINativeMethod *m,
                                jint n) {
  UNUSED_ENV; (void)c; (void)m; (void)n;
  return 0;
}
static jint jni_UnregisterNatives(JNIEnv *env, jclass c) { UNUSED_ENV; (void)c; return 0; }

static jobject jni_NewDirectByteBuffer(JNIEnv *env, void *address, jlong capacity) {
  UNUSED_ENV; (void)capacity;
  return (jobject)address;
}
static void *jni_GetDirectBufferAddress(JNIEnv *env, jobject buffer) {
  UNUSED_ENV;
  return (void *)buffer;
}
static jlong jni_GetDirectBufferCapacity(JNIEnv *env, jobject buffer) {
  UNUSED_ENV; (void)buffer;
  return 0;
}
static jobjectRefType jni_GetObjectRefType(JNIEnv *env, jobject o) {
  UNUSED_ENV;
  return o ? JNIGlobalRefType : JNIInvalidRefType;
}

static jobject jni_AllocObject(JNIEnv *env, jclass c) { UNUSED_ENV; (void)c; return NULL; }
static jobject jni_NewObject(JNIEnv *env, jclass c, jmethodID m, ...) {
  UNUSED_ENV; (void)c; (void)m;
  return NULL;
}
static jobject jni_NewObjectV(JNIEnv *env, jclass c, jmethodID m, va_list va) {
  UNUSED_ENV; (void)c; (void)m; (void)va;
  return NULL;
}
static jobject jni_NewObjectA(JNIEnv *env, jclass c, jmethodID m, const jvalue *v) {
  UNUSED_ENV; (void)c; (void)m; (void)v;
  return NULL;
}
static jclass jni_DefineClass(JNIEnv *env, const char *name, jobject loader,
                              const jbyte *buf, jsize len) {
  UNUSED_ENV; (void)loader; (void)buf; (void)len;
  return intern_class(name);
}
static jmethodID jni_FromReflectedMethod(JNIEnv *env, jobject m) {
  UNUSED_ENV; (void)m; return NULL;
}
static jfieldID jni_FromReflectedField(JNIEnv *env, jobject f) {
  UNUSED_ENV; (void)f; return NULL;
}
static jobject jni_ToReflectedMethod(JNIEnv *env, jclass c, jmethodID m, jboolean s) {
  UNUSED_ENV; (void)c; (void)m; (void)s; return NULL;
}
static jobject jni_ToReflectedField(JNIEnv *env, jclass c, jfieldID f, jboolean s) {
  UNUSED_ENV; (void)c; (void)f; (void)s; return NULL;
}

static jint jni_GetJavaVM(JNIEnv *env, JavaVM **vm);

static struct JNINativeInterface g_native_interface;
static struct JNIInvokeInterface g_invoke_interface;

JNIEnv jni_env;
JavaVM jni_vm;

static jint jni_GetJavaVM(JNIEnv *env, JavaVM **vm) {
  UNUSED_ENV;
  if (vm) *vm = &jni_vm;
  return JNI_OK;
}

/* JavaVM side. Threads created by the guest already have a bionic TLS block
 * from pthread_shim, so attaching is bookkeeping only. */
static jint vm_GetEnv(JavaVM *vm, void **out, jint version) {
  (void)vm; (void)version;
  if (out) *out = &jni_env;
  return JNI_OK;
}
static jint vm_AttachCurrentThread(JavaVM *vm, JNIEnv **out, void *args) {
  (void)vm; (void)args;
  if (out) *out = &jni_env;
  return JNI_OK;
}
static jint vm_AttachCurrentThreadAsDaemon(JavaVM *vm, JNIEnv **out, void *args) {
  return vm_AttachCurrentThread(vm, out, args);
}
static jint vm_DetachCurrentThread(JavaVM *vm) { (void)vm; return JNI_OK; }
static jint vm_DestroyJavaVM(JavaVM *vm) { (void)vm; return JNI_OK; }

void jni_init(void) {
  mutexInit(&g_class_lock);
  g_context = obj_new(OBJ_CONTEXT);

  struct JNINativeInterface *n = &g_native_interface;
  memset(n, 0, sizeof *n);

  n->GetVersion = jni_GetVersion;
  n->DefineClass = jni_DefineClass;
  n->FindClass = jni_FindClass;
  n->FromReflectedMethod = jni_FromReflectedMethod;
  n->FromReflectedField = jni_FromReflectedField;
  n->ToReflectedMethod = jni_ToReflectedMethod;
  n->GetSuperclass = jni_GetSuperclass;
  n->IsAssignableFrom = jni_IsAssignableFrom;
  n->ToReflectedField = jni_ToReflectedField;
  n->Throw = jni_Throw;
  n->ThrowNew = jni_ThrowNew;
  n->ExceptionOccurred = jni_ExceptionOccurred;
  n->ExceptionDescribe = jni_ExceptionDescribe;
  n->ExceptionClear = jni_ExceptionClear;
  n->FatalError = jni_FatalError;
  n->PushLocalFrame = jni_PushLocalFrame;
  n->PopLocalFrame = jni_PopLocalFrame;
  n->NewGlobalRef = jni_NewGlobalRef;
  n->DeleteGlobalRef = jni_DeleteGlobalRef;
  n->DeleteLocalRef = jni_DeleteLocalRef;
  n->IsSameObject = jni_IsSameObject;
  n->NewLocalRef = jni_NewLocalRef;
  n->EnsureLocalCapacity = jni_EnsureLocalCapacity;
  n->AllocObject = jni_AllocObject;
  n->NewObject = jni_NewObject;
  n->NewObjectV = jni_NewObjectV;
  n->NewObjectA = jni_NewObjectA;
  n->GetObjectClass = jni_GetObjectClass;
  n->IsInstanceOf = jni_IsInstanceOf;
  n->GetMethodID = jni_GetMethodID;

#define BIND_CALLS(Kind)                                    \
  n->Call##Kind##Method = jni_Call##Kind##Method;           \
  n->Call##Kind##MethodV = jni_Call##Kind##MethodV;         \
  n->Call##Kind##MethodA = jni_Call##Kind##MethodA;         \
  n->CallNonvirtual##Kind##Method = jni_CallNonvirtual##Kind##Method;   \
  n->CallNonvirtual##Kind##MethodV = jni_CallNonvirtual##Kind##MethodV; \
  n->CallNonvirtual##Kind##MethodA = jni_CallNonvirtual##Kind##MethodA; \
  n->CallStatic##Kind##Method = jni_CallStatic##Kind##Method;           \
  n->CallStatic##Kind##MethodV = jni_CallStatic##Kind##MethodV;         \
  n->CallStatic##Kind##MethodA = jni_CallStatic##Kind##MethodA;

  BIND_CALLS(Object)
  BIND_CALLS(Boolean)
  BIND_CALLS(Byte)
  BIND_CALLS(Char)
  BIND_CALLS(Short)
  BIND_CALLS(Int)
  BIND_CALLS(Long)
  BIND_CALLS(Float)
  BIND_CALLS(Double)
  BIND_CALLS(Void)
#undef BIND_CALLS

  n->GetFieldID = jni_GetFieldID;
  n->GetStaticMethodID = jni_GetStaticMethodID;
  n->GetStaticFieldID = jni_GetStaticFieldID;

#define BIND_FIELDS(Kind)                                 \
  n->Get##Kind##Field = jni_Get##Kind##Field;             \
  n->Set##Kind##Field = jni_Set##Kind##Field;             \
  n->GetStatic##Kind##Field = jni_GetStatic##Kind##Field; \
  n->SetStatic##Kind##Field = jni_SetStatic##Kind##Field;

  BIND_FIELDS(Object)
  BIND_FIELDS(Boolean)
  BIND_FIELDS(Byte)
  BIND_FIELDS(Char)
  BIND_FIELDS(Short)
  BIND_FIELDS(Int)
  BIND_FIELDS(Long)
  BIND_FIELDS(Float)
  BIND_FIELDS(Double)
#undef BIND_FIELDS

  n->NewString = jni_NewString;
  n->GetStringLength = jni_GetStringLength;
  n->GetStringChars = jni_GetStringChars;
  n->ReleaseStringChars = jni_ReleaseStringChars;
  n->NewStringUTF = jni_NewStringUTF;
  n->GetStringUTFLength = jni_GetStringUTFLength;
  n->GetStringUTFChars = jni_GetStringUTFChars;
  n->ReleaseStringUTFChars = jni_ReleaseStringUTFChars;
  n->GetArrayLength = jni_GetArrayLength;
  n->NewObjectArray = jni_NewObjectArray;
  n->GetObjectArrayElement = jni_GetObjectArrayElement;
  n->SetObjectArrayElement = jni_SetObjectArrayElement;

#define BIND_ARRAYS(Kind)                                             \
  n->New##Kind##Array = jni_New##Kind##Array;                         \
  n->Get##Kind##ArrayElements = jni_Get##Kind##ArrayElements;         \
  n->Release##Kind##ArrayElements = jni_Release##Kind##ArrayElements; \
  n->Get##Kind##ArrayRegion = jni_Get##Kind##ArrayRegion;             \
  n->Set##Kind##ArrayRegion = jni_Set##Kind##ArrayRegion;

  BIND_ARRAYS(Boolean)
  BIND_ARRAYS(Byte)
  BIND_ARRAYS(Char)
  BIND_ARRAYS(Short)
  BIND_ARRAYS(Int)
  BIND_ARRAYS(Long)
  BIND_ARRAYS(Float)
  BIND_ARRAYS(Double)
#undef BIND_ARRAYS

  n->RegisterNatives = jni_RegisterNatives;
  n->UnregisterNatives = jni_UnregisterNatives;
  n->MonitorEnter = jni_MonitorEnter;
  n->MonitorExit = jni_MonitorExit;
  n->GetJavaVM = jni_GetJavaVM;
  n->GetStringRegion = jni_GetStringRegion;
  n->GetStringUTFRegion = jni_GetStringUTFRegion;
  n->GetPrimitiveArrayCritical = jni_GetPrimitiveArrayCritical;
  n->ReleasePrimitiveArrayCritical = jni_ReleasePrimitiveArrayCritical;
  n->GetStringCritical = jni_GetStringCritical;
  n->ReleaseStringCritical = jni_ReleaseStringCritical;
  n->NewWeakGlobalRef = jni_NewWeakGlobalRef;
  n->DeleteWeakGlobalRef = jni_DeleteWeakGlobalRef;
  n->ExceptionCheck = jni_ExceptionCheck;
  n->NewDirectByteBuffer = jni_NewDirectByteBuffer;
  n->GetDirectBufferAddress = jni_GetDirectBufferAddress;
  n->GetDirectBufferCapacity = jni_GetDirectBufferCapacity;
  n->GetObjectRefType = jni_GetObjectRefType;

  struct JNIInvokeInterface *v = &g_invoke_interface;
  memset(v, 0, sizeof *v);
  v->DestroyJavaVM = vm_DestroyJavaVM;
  v->AttachCurrentThread = vm_AttachCurrentThread;
  v->DetachCurrentThread = vm_DetachCurrentThread;
  v->GetEnv = vm_GetEnv;
  v->AttachCurrentThreadAsDaemon = vm_AttachCurrentThreadAsDaemon;

  jni_env = n;
  jni_vm = v;
}
