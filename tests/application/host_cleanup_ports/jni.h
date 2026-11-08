/* First-party typed subset used by the shipping JNI host, never a JVM ABI. */
#ifndef JCE_TEST_HOST_JNI_H
#define JCE_TEST_HOST_JNI_H
#include <stdint.h>
#define JNIEXPORT
#define JNICALL
typedef int jint;
typedef unsigned char jboolean;
typedef int64_t jlong;
typedef void *jobject;
typedef jobject jclass;
typedef jobject jstring;
typedef jobject jmethodID;
typedef const struct JceTestJniTable *JNIEnv;
struct JceTestJniTable {
    jclass (*FindClass)(JNIEnv *, const char *);
    jmethodID (*GetStaticMethodID)(JNIEnv *, jclass, const char *, const char *);
    jstring (*NewStringUTF)(JNIEnv *, const char *);
    jobject (*CallStaticObjectMethod)(JNIEnv *, jclass, jmethodID, ...);
    void (*DeleteLocalRef)(JNIEnv *, jobject);
    const char *(*GetStringUTFChars)(JNIEnv *, jstring, void *);
    void (*ReleaseStringUTFChars)(JNIEnv *, jstring, const char *);
    jint (*ThrowNew)(JNIEnv *, jclass, const char *);
    jboolean (*ExceptionCheck)(JNIEnv *);
};
#endif
