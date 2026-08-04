#pragma once

#include <binder/Parcel.h>

namespace android {

// Forward declaration – we will not define this class
class ICameraServiceExt;

class CameraServiceExtFactory {
public:
    // Returns the OxygenOS extension factory object exactly as getExtFactoryImpl()
    // produced it. libcsextimpl performs C++ virtual dispatch on this pointer, so
    // it must be the object itself, never a synthetic function table.
    static void* getInstance();
    static int onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags);
    virtual ~CameraServiceExtFactory();

private:
    static void ensureLoaded();
    static void* sFactoryObject;    // getExtFactoryImpl() result; its first word is a vptr
    static void* sExtObject;        // factory->vtable[0](factory)
    static int (*sOnTransactFunc)(void*, uint32_t, const Parcel&, Parcel*, uint32_t);
};

} // namespace android
