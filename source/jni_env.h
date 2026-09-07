/* A minimal in-process JNI environment.
 *
 * The guest was built to talk to a Java activity; nothing here interprets
 * bytecode. Class and method lookups resolve to small integer ids that
 * java_bridge.c answers directly.
 */

#ifndef BSNX_JNI_ENV_H
#define BSNX_JNI_ENV_H

#include <stdarg.h>

#include "jni.h"

extern JNIEnv jni_env;
extern JavaVM jni_vm;

void jni_init(void);

/* The stand-in for the Java activity instance. Valid after jni_init. */
jobject jni_context_object(void);

/* Uniform argument cursor so a bridge handler can serve both the varargs
 * (Call*MethodV) and jvalue-array (Call*MethodA) call shapes. */
typedef struct {
  int from_array;
  va_list *va;
  const jvalue *values;
  int index;
} JArgs;

jboolean jargs_boolean(JArgs *args);
jint jargs_int(JArgs *args);
jlong jargs_long(JArgs *args);
jfloat jargs_float(JArgs *args);
jdouble jargs_double(JArgs *args);
jobject jargs_object(JArgs *args);

typedef enum {
  JK_VOID,
  JK_BOOLEAN,
  JK_BYTE,
  JK_CHAR,
  JK_SHORT,
  JK_INT,
  JK_LONG,
  JK_FLOAT,
  JK_DOUBLE,
  JK_OBJECT,
} JKind;

typedef struct {
  int id;
  const char *name;
  JKind kind;
} JMemberDesc;

/* Supplied by java_bridge.c. */
extern const JMemberDesc java_methods[];
extern const int java_method_count;
extern const JMemberDesc java_fields[];
extern const int java_field_count;

void java_call_void(int id, JArgs *args);
jboolean java_call_boolean(int id, JArgs *args);
jint java_call_int(int id, JArgs *args);
jlong java_call_long(int id, JArgs *args);
jfloat java_call_float(int id, JArgs *args);
jdouble java_call_double(int id, JArgs *args);
jobject java_call_object(int id, JArgs *args);
jobject java_get_object_field(int id);
jint java_get_int_field(int id);

/* Object helpers the bridge and the bitmap layer build on. */
jstring jni_new_string(const char *utf8);
const char *jni_string_utf8(jstring string);
jobject jni_new_float_array(const float *values, int count);
const float *jni_float_array_data(jobject array);
int jni_array_length(jobject array);
const char *jni_object_array_utf8(jobject array, int index);

/* Backing store for an android.graphics.Bitmap the guest will lock. */
typedef struct {
  int width;
  int height;
  int stride;
  void *pixels; /* RGBA_8888 */
} JBitmap;

jobject jni_new_bitmap(int width, int height);
JBitmap *jni_bitmap(jobject object);

#endif
