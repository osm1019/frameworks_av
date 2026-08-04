#include "CameraServiceExtFactory.h"
#include <dlfcn.h>
#include <log/log.h>
#include <mutex>

namespace android {

void* CameraServiceExtFactory::sFactoryObject = nullptr;
void* CameraServiceExtFactory::sExtObject = nullptr;
int (*CameraServiceExtFactory::sOnTransactFunc)(void*, uint32_t, const Parcel&, Parcel*, uint32_t) = nullptr;

namespace {

// libcsextimpl re-enters getInstance() from inside its own onTransact(), so the
// guard has to be recursive.
std::recursive_mutex& extLock() {
    static std::recursive_mutex lock;
    return lock;
}

bool gLoadAttempted = false;

} // namespace

void CameraServiceExtFactory::ensureLoaded() {
    if (gLoadAttempted) return;
    gLoadAttempted = true;

    const char* libPath = "system_ext/lib64/libcsextimpl.so";
    void* handle = dlopen(libPath, RTLD_NOW);
    if (handle == nullptr) {
        ALOGE("CameraServiceExtFactory: dlopen failed: %s", dlerror());
        return;
    }
    ALOGI("CameraServiceExtFactory: dlopen succeeded, handle=%p", handle);

    typedef void* (*GetFactoryFunc)();
    GetFactoryFunc getExtFactoryImpl = (GetFactoryFunc)dlsym(handle, "getExtFactoryImpl");
    if (getExtFactoryImpl == nullptr) {
        ALOGE("CameraServiceExtFactory: dlsym getExtFactoryImpl failed: %s", dlerror());
        dlclose(handle);
        return;
    }

    // getExtFactoryImpl() hands back a heap object whose only member is its
    // vptr, and libcsextimpl expects to receive that pointer back verbatim from
    // getInstance(): at CameraServiceExtImpl::onTransact it does
    //
    //     bl  getInstance
    //     ldr x8, [x0]      // vtable
    //     ldr x8, [x8]      // vtable[0]
    //     blr x8            // this == x0
    //
    // i.e. ordinary C++ virtual dispatch on the factory. Wrapping the pointer
    // in a synthetic one-entry "function table" adds an indirection, so the
    // second load reads the callee's own prologue (paciasp; sub sp, sp, #imm)
    // and branches to it -- an unaligned PC, which the kernel reports as
    // SIGBUS/BUS_ADRALN and takes cameraserver down on every camera open.
    sFactoryObject = getExtFactoryImpl();
    if (sFactoryObject == nullptr) {
        ALOGE("CameraServiceExtFactory: getExtFactoryImpl returned null");
        dlclose(handle);
        return;
    }
    ALOGI("CameraServiceExtFactory: factory object at %p", sFactoryObject);

    sOnTransactFunc = (int (*)(void*, uint32_t, const Parcel&, Parcel*, uint32_t))
        dlsym(handle, "_ZN7android20CameraServiceExtImpl10onTransactEjRKNS_6ParcelEPS1_j");
    if (sOnTransactFunc == nullptr) {
        ALOGE("CameraServiceExtFactory: dlsym onTransact failed: %s", dlerror());
    } else {
        ALOGI("CameraServiceExtFactory: onTransact found at %p", sOnTransactFunc);
    }
}

void* CameraServiceExtFactory::getInstance() {
    std::lock_guard<std::recursive_mutex> guard(extLock());
    ensureLoaded();
    return sFactoryObject;   // may be null
}

int CameraServiceExtFactory::onTransact(uint32_t code, const Parcel& data, Parcel* reply,
                                        uint32_t flags) {
    void* extObject = nullptr;
    int (*onTransactFunc)(void*, uint32_t, const Parcel&, Parcel*, uint32_t) = nullptr;

    {
        std::lock_guard<std::recursive_mutex> guard(extLock());
        ensureLoaded();

        if (sFactoryObject == nullptr || sOnTransactFunc == nullptr) {
            ALOGE("CameraServiceExtFactory::onTransact: extension not loaded");
            return -1;
        }

        if (sExtObject == nullptr) {
            // factory->vtable[0](factory) -- a virtual member function, so the
            // factory itself has to be passed as 'this'.
            void* const* vtable = *reinterpret_cast<void* const* const*>(sFactoryObject);
            if (vtable == nullptr || vtable[0] == nullptr) {
                ALOGE("CameraServiceExtFactory: factory has no vtable slot 0");
                return -1;
            }
            typedef void* (*GetExtObjectFunc)(void*);
            sExtObject = reinterpret_cast<GetExtObjectFunc>(vtable[0])(sFactoryObject);
            if (sExtObject == nullptr) {
                ALOGE("CameraServiceExtFactory: factory returned null");
                return -1;
            }
            ALOGI("CameraServiceExtFactory: real extension object at %p", sExtObject);
        }

        extObject = sExtObject;
        onTransactFunc = sOnTransactFunc;
    }

    // Called without the lock held: the extension calls back into getInstance().
    return onTransactFunc(extObject, code, data, reply, flags);
}

CameraServiceExtFactory::~CameraServiceExtFactory() {
    // No cleanup needed – the extension library manages its own singleton.
    ALOGV("CameraServiceExtFactory destructor (stub)");
}

} // namespace android
