#include <jni.h>
#include <string>
#include "radio_control/FakeControl.h"
#include "radio_control/MicrohardControl.h"
#include "radio_control/HelixControl.h"
#include "radio_control/PoplarControl.h"
#include "radio_control/DTCControl.h"


#define ENABLE_VRTS_SERVER 1

#if (ENABLE_VRTS_SERVER)
#include "VrtsVideoServer.h"
#endif

namespace jni_wrap {
    std::shared_ptr<radio_control::RadioControl> radio_control = nullptr;
    std::shared_ptr<VrtsVideoServer> vrts_server = nullptr;

    std::string jstring2string(JNIEnv *env, jstring jStr) {
        if (!jStr)
            return "";

        const jclass stringClass = env->GetObjectClass(jStr);
        const jmethodID getBytes = env->GetMethodID(stringClass, "getBytes", "(Ljava/lang/String;)[B");
        const jbyteArray stringJbytes = (jbyteArray) env->CallObjectMethod(jStr, getBytes, env->NewStringUTF("UTF-8"));

        size_t length = (size_t) env->GetArrayLength(stringJbytes);
        jbyte* pBytes = env->GetByteArrayElements(stringJbytes, NULL);

        std::string ret = std::string((char *)pBytes, length);
        env->ReleaseByteArrayElements(stringJbytes, pBytes, JNI_ABORT);

        env->DeleteLocalRef(stringJbytes);
        env->DeleteLocalRef(stringClass);
        return ret;
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_stringFromJNI(
        JNIEnv* env,
        jobject /* this */) {
    std::string hello = "DISABLED";
    if (jni_wrap::radio_control != nullptr) {
        if (jni_wrap::radio_control->GetModel() == radio_control::RadioModel::FAKE) {
            hello = "FAKE";
        } else if (jni_wrap::radio_control->GetModel() == radio_control::RadioModel::HELIX2X2) {
            hello = "HELIX 2x2";
        } else if (jni_wrap::radio_control->GetModel() == radio_control::RadioModel::PMDDL2450) {
            hello = "PDDL2450";
        } else if (jni_wrap::radio_control->GetModel() == radio_control::RadioModel::PMDDL2450) {
            hello = "PDDL1800";
        } else if (jni_wrap::radio_control->GetModel() == radio_control::RadioModel::PMDDL2450) {
            hello = "SBS 356-380 MHz";
        }  else if (jni_wrap::radio_control->GetModel() == radio_control::RadioModel::DTC) {
            hello = "DTC SDR6 - 2100/2500 MHz";
        }

        if (jni_wrap::radio_control->GetRadioState() == radio_control::RadioState::BOOTING) {
            hello = std::string(hello + " : Initializing!");
        }
        if (jni_wrap::radio_control->GetRadioState() == radio_control::RadioState::CONFIGURING) {
            hello = std::string(hello + " : Configuring!");
        }
        if (jni_wrap::radio_control->GetRadioState() == radio_control::RadioState::REMOVED) {
            hello = std::string("REMOVED");
        }
    }
    return env->NewStringUTF(hello.c_str());
}
extern "C" JNIEXPORT jint JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_enableRadio(JNIEnv* env, jobject /* this */,
                                                                         jboolean enable, jint radio) {
    if (enable) {
        if (radio == 0) {
            jni_wrap::radio_control = std::make_shared<radio_control::FakeControl>(true);
        } else if (radio == 1) {
            jni_wrap::radio_control = std::make_shared<radio_control::MicrohardControl>(true);
        } else if (radio == 2) {
            jni_wrap::radio_control = std::make_shared<radio_control::HelixControl>(true);
        } else if (radio == 3) {
            jni_wrap::radio_control = std::make_shared<radio_control::PoplarControl>(true);
        } else if (radio == 4) {
            jni_wrap::radio_control = std::make_shared<radio_control::DTCControl>(true, "", "192.168.20.104", "192.168.20.4");
        }
    } else {
        jni_wrap::radio_control = nullptr;
    }

#if (ENABLE_VRTS_SERVER)
    if (jni_wrap::radio_control != nullptr) {
        jni_wrap::vrts_server = std::make_shared<VrtsVideoServer>("0.0.0.0", 30000);
        //TODO breaks when VRTS used with RAS-A configured vehicle IP different from default
        jni_wrap::vrts_server->setDownstreamUrl("vrts://192.168.20.30:20000");

        // Start VRTS video server
        jni_wrap::vrts_server->startup(10000);
    } else {
        if (jni_wrap::vrts_server->running())
            jni_wrap::vrts_server->shutdown();
        jni_wrap::vrts_server = nullptr;
    }
#endif
    return 0;
}


extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_isModemConnected(JNIEnv *env, jobject thiz) {
    if (jni_wrap::radio_control != nullptr && jni_wrap::radio_control->GetRadioState() == radio_control::RadioState::CONNECTED) {
        // Assuming GetRSSI is a method in MicrohardControl or other connected control classes
        int rssi = jni_wrap::radio_control->GetRSSI();  // Replace with correct method call if different
        if (rssi > -100) {  // Assuming -100 dBm as threshold for "connected" quality
            return JNI_TRUE;
        }
    }
    return JNI_FALSE;
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_getRSSI(JNIEnv *env, jobject thiz) {
    if (jni_wrap::radio_control != nullptr && jni_wrap::radio_control->GetRadioState() == radio_control::RadioState::CONNECTED) {
        return jni_wrap::radio_control->GetRSSI();  // Assuming GetRSSI retrieves the RSSI in dBm
    }
    return -1; // Return -1 if not connected or RSSI unavailable
}
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_getSupportedFreqs(JNIEnv* env, jobject /* this */) {
    jclass intClass = env->FindClass("java/lang/Integer");
    std::vector<int> f;
    if (jni_wrap::radio_control != nullptr) {
        for (auto obj : jni_wrap::radio_control->GetSupportedFreqAndMaxBWPer()) {
            f.push_back(std::get<0>(obj));
        }
    } else {
        f.push_back(0);
    }

    jobjectArray freqs = env->NewObjectArray(f.size(), intClass, 0);
    int i = 0;
    for (int freq : f) {
        jmethodID integerConstructor = env->GetMethodID(intClass, "<init>", "(I)V");
        jobject wrappedInt = env->NewObject(intClass, integerConstructor, static_cast<jint>(freq));
        env->SetObjectArrayElement(freqs, i, wrappedInt);
        i++;
    }
    return(freqs);
}
extern "C" JNIEXPORT jintArray JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_scanChannels(JNIEnv* env, jobject /* this */, jint sort, jint count, jfloat bw) {
    // Ensure jni_wrap::radio_control is of type MicrohardControl
    if (auto control = std::dynamic_pointer_cast<radio_control::MicrohardControl>(jni_wrap::radio_control)) {
        // Call the ScanChannels function with the provided arguments
        std::vector<int> channels = control->ScanChannels(static_cast<int>(sort), static_cast<int>(count), static_cast<float>(bw));

        // Convert std::vector<int> to jintArray to return to Java
        jintArray result = env->NewIntArray(channels.size());
        if (result == nullptr) {
            // Return an empty array if memory allocation fails
            return env->NewIntArray(0);
        }
        env->SetIntArrayRegion(result, 0, channels.size(), channels.data());
        return result;
    }
    // Return an empty array if jni_wrap::radio_control is not of type MicrohardControl
    return env->NewIntArray(0);
}

extern "C" JNIEXPORT jobjectArray JNICALL
        Java_com_example_vantageradioui_ui_main_RadioControlFragment_getSupportedBWs(JNIEnv* env, jobject /* this */) {
    jclass floatClass = env->FindClass("java/lang/Float");
    std::vector<float> bw;
    if (jni_wrap::radio_control != nullptr) {
        for (float b : jni_wrap::radio_control->GetSupportedBWs()) {
            bw.push_back(b);
        }
    } else {
        bw.push_back(0);
    }

    jobjectArray bws = env->NewObjectArray(bw.size(), floatClass, 0);
    int i = 0;
    for (int freq : bw) {
        jmethodID floatConstructor = env->GetMethodID(floatClass, "<init>", "(F)V");
        jobject wrappedFloat = env->NewObject(floatClass, floatConstructor, static_cast<jfloat>(freq));
        env->SetObjectArrayElement(bws, i, wrappedFloat);
        i++;
    }
    return(bws);
}


extern "C"
JNIEXPORT jstring JNICALL
Java_com_example_vantageradioui_ui_main_ScrollingFragment_getRadioLog(JNIEnv *env, jobject thiz) {
    int entries = radio_control::get_vrc_log_len();
    std::string output = "";

    // Retrieve and append standard radio logs
    for (int i = 0; i < entries; i++) {
        output += radio_control::get_vrc_log();
    }

    const char *str = output.c_str();
    jobject bb = env->NewDirectByteBuffer((void *) str, strlen(str));

    jclass cls_Charset = env->FindClass("java/nio/charset/Charset");
    jmethodID mid_Charset_forName = env->GetStaticMethodID(cls_Charset, "forName", "(Ljava/lang/String;)Ljava/nio/charset/Charset;");
    jobject charset = env->CallStaticObjectMethod(cls_Charset, mid_Charset_forName, env->NewStringUTF("UTF-8"));

    jmethodID mid_Charset_decode = env->GetMethodID(cls_Charset, "decode", "(Ljava/nio/ByteBuffer;)Ljava/nio/CharBuffer;");
    jobject cb = env->CallObjectMethod(charset, mid_Charset_decode, bb);
    env->DeleteLocalRef(bb);

    jclass cls_CharBuffer = env->FindClass("java/nio/CharBuffer");
    jmethodID mid_CharBuffer_toString = env->GetMethodID(cls_CharBuffer, "toString", "()Ljava/lang/String;");

    jstring parsed = static_cast<jstring>(env->CallObjectMethod(cb, mid_CharBuffer_toString));

    return parsed;
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_getFreq(JNIEnv *env, jobject thiz) {
    return jni_wrap::radio_control->GetCurrentFrequency();
}



extern "C"
JNIEXPORT jfloat JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_getBW(JNIEnv *env, jobject thiz) {
    return jni_wrap::radio_control->GetCurrentBW();
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_getPower(JNIEnv *env, jobject thiz) {
    return jni_wrap::radio_control->GetCurrentPower();

}
extern "C"
JNIEXPORT jstring JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_getNetworkPassword(JNIEnv *env, jobject thiz) {


    // Retrieve the password as a std::string
    std::string password = jni_wrap::radio_control->GetNetworkPassword();

    // Convert std::string to jstring
    return env->NewStringUTF(password.c_str());
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_getNetworkID(JNIEnv *env, jobject thiz) {


    // Retrieve the password as a std::string
    std::string password = jni_wrap::radio_control->GetNetworkID();

    // Convert std::string to jstring
    return env->NewStringUTF(password.c_str());
}



extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_isConnected(JNIEnv *env, jobject thiz) {
    return jni_wrap::radio_control->IsConnected();
}

extern "C"
JNIEXPORT void JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_setFreqBW(JNIEnv *env, jobject thiz,
                                                                       jint freq, jfloat bw) {
    jni_wrap::radio_control->SetFrequencyAndBW(freq, bw);

}
extern "C"
JNIEXPORT void JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_SetOutputPower(JNIEnv *env, jobject thiz,
                                                                       jint value ) {
    jni_wrap::radio_control->SetOutputPower(value);

}





extern "C"
JNIEXPORT void JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_setNetworkName(JNIEnv *env, jobject thiz,
                                                                            jstring name) {
    jni_wrap::radio_control->SetNetworkID(jni_wrap::jstring2string(env, name));
}

extern "C"
JNIEXPORT void JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_setNetworkPassword(JNIEnv *env,jobject thiz, jstring pass) {
    jni_wrap::radio_control->SetNetworkPassword(jni_wrap::jstring2string(env, pass));
}

extern "C"
JNIEXPORT void JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_applySettings(JNIEnv *env, jobject thiz) {
    if (jni_wrap::radio_control->GetModel() == radio_control::RadioModel::HELIX2X2) {
        jni_wrap::radio_control->SetOutputPower((jint) 30);
    }
    jni_wrap::radio_control->ApplySettings();
}extern "C"
JNIEXPORT jstring JNICALL
Java_com_example_vantageradioui_ui_main_RadioControlFragment_getState(JNIEnv *env, jobject thiz) {
    std::string status("UNKNOWN");
    if (jni_wrap::radio_control != nullptr) {
        if (jni_wrap::radio_control->GetRadioState() == radio_control::RadioState::BOOTING) {
            status = "BOOTING";
            return env->NewStringUTF(status.c_str());
        }
        if (jni_wrap::radio_control->GetRadioState() == radio_control::RadioState::CONFIGURING) {
            status = "CONFIGURING";
            return env->NewStringUTF(status.c_str());
        }
        if (jni_wrap::radio_control->GetRadioState() == radio_control::RadioState::DISCONNECTED) {
            status = "DISCONNECTED";
            return env->NewStringUTF(status.c_str());
        }
        if (jni_wrap::radio_control->GetRadioState() == radio_control::RadioState::CONNECTED) {
            status = "CONNECTED";
            return env->NewStringUTF(status.c_str());
        }
        if (jni_wrap::radio_control->GetRadioState() == radio_control::RadioState::REMOVED) {
            status = "REMOVED";
            return env->NewStringUTF(status.c_str());
        }
    }
    return env->NewStringUTF(status.c_str());
}