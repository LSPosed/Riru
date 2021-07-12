#include <dlfcn.h>
#include <android_prop.h>
#include <pthread.h>
#include <rirud.h>
#include "misc.h"
#include "jni_hooks.h"
#include "logging.h"
#include "module.h"
#include "hide_utils.h"
#include "magisk.h"
#include "entry.h"

static void *self_handle;
static bool self_unload_allowed;

struct SelfUnloadGuard {

    SelfUnloadGuard() {
        pthread_mutex_init(&mutex_, nullptr);
    }

    ~SelfUnloadGuard() {
        LOGD("self unload lock (destructor)");
        pthread_mutex_lock(&mutex_);

        LOGD("self unload");

        timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
        nanosleep(&ts, nullptr);
    }

    struct Holder {
        explicit Holder(pthread_mutex_t *mutex) : mutex_(mutex) {
            LOGD("self unload lock (holder constructor)");
            pthread_mutex_lock(mutex_);
        }

        Holder(Holder &&other) noexcept: mutex_(other.mutex_) {
            other.mutex_ = nullptr;
        }

        ~Holder() {
            if (mutex_) {
                pthread_mutex_unlock(mutex_);
                LOGD("self unload unlock (holder destructor)");
            }
        }

    private:
        pthread_mutex_t *mutex_;

    public:
        Holder(const Holder &) = delete;

        void operator=(const Holder &) = delete;
    };

    auto hold() { return Holder(&mutex_); };

private:
    pthread_mutex_t mutex_{};
} self_unload_guard;

static void SelfUnload() {
    LOGD("attempt to self unload");

    [[maybe_unused]] auto holder = self_unload_guard.hold();

    pthread_t thread;
    pthread_create(&thread, nullptr, (void *(*)(void *)) &dlclose, self_handle);
    pthread_detach(thread);
}

bool Entry::IsSelfUnloadAllowed() {
    return self_unload_allowed;
}

void Entry::Unload(jboolean is_child_zygote) {
    self_unload_allowed = true;

    for (auto module : Modules::Get()) {
        if (strcmp(module->id, MODULE_NAME_CORE) == 0) {
            continue;
        }

        if (module->allowUnload()) {
            LOGD("%s: unload", module->id);
            module->unload();
        } else {
            if (module->apiVersion >= 25)
                LOGD("%s: unload is not allow for this process", module->id);
            else {
                LOGD("%s: unload is not supported by module (API < 25), self unload is also disabled", module->id);
                self_unload_allowed = false;
            }
        }
    }

    Hide::HideFromSoList();

    // Child zygote (webview zyote or app zygote) has no "execmem" permission
    if (AndroidProp::GetApiLevel() < 29 && !is_child_zygote) {
        Hide::HideFromMaps();
    }

    if (self_unload_allowed) {
        SelfUnload();
    }
}

extern "C" __attribute__((visibility("default"))) __attribute__((used)) void init(void *handle, const char* magisk_path) {
    self_handle = handle;

    Magisk::SetPath(magisk_path);
    Hide::PrepareMapsHideLibrary();
    JNI::InstallHooks();
    Modules::Load();

    std::vector<rirud::Module> modules;
    for (auto it : Modules::Get()) {
        if (strcmp(it->id, MODULE_NAME_CORE) == 0) {
            continue;
        }

        auto module = rirud::Module{};

        strncpy(module.id, it->id, sizeof(module.id));
        module.apiVersion = it->apiVersion;
        module.version = it->version;
        strncpy(module.versionName, it->versionName, sizeof(module.versionName));
        module.supportHide = it->supportHide;

        modules.emplace_back(module);
    }
    rirud::WriteModules(modules);
    rirud::CloseSocket();
}
