/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "powerhal-libperfmgr"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android/binder_ibinder_platform.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <perfmgr/HintManager.h>

#include <thread>

#include "Power.h"
#include "PowerExt.h"
#include "PowerSessionManager.h"

using aidl::google::hardware::power::impl::pixel::Power;
using aidl::google::hardware::power::impl::pixel::PowerExt;
using aidl::google::hardware::power::impl::pixel::PowerHintMonitor;
using aidl::google::hardware::power::impl::pixel::PowerSessionManager;
using ::android::perfmgr::HintManager;

constexpr std::string_view kPowerHalInitProp("vendor.powerhal.init");
#ifdef USE_POWER_PLANS
constexpr std::string_view kPowerPlanProp("vendor.powerhal.profile");
constexpr std::string_view kConfigDefaultFileName("default.json");
constexpr std::string_view kConfigPowerSave("powersave.json");
constexpr std::string_view kConfigBalanced("balanced.json");
constexpr std::string_view kConfigPerformance("performance.json");
#else
constexpr std::string_view kConfigProperty("vendor.powerhal.config");
constexpr std::string_view kConfigDefaultFileName("powerhint.json");
#endif

int main() {
    std::string config_path = "/vendor/etc/";
#ifdef USE_POWER_PLANS
    std::string PowerPlan = android::base::GetProperty(kPowerPlanProp.data(), "");
    if (PowerPlan.empty()) {
        LOG(WARNING) << "No power plan is set, using default config";
        config_path.append(kConfigDefaultFileName.data());
    } else {
        if (PowerPlan == "powersave") {
            config_path.append(kConfigPowerSave.data());
        } else if (PowerPlan == "balanced") {
            config_path.append(kConfigBalanced.data());
        } else if (PowerPlan == "performance") {
            config_path.append(kConfigPerformance.data());
        } else {
            LOG(WARNING) << "Unknown power plan: " << PowerPlan << ", using default config";
            config_path.append(kConfigDefaultFileName.data());
        }
    }
#else
    config_path.append(
            android::base::GetProperty(kConfigProperty.data(), kConfigDefaultFileName.data()));
#endif

    // Parse config but do not start the looper
    std::shared_ptr<HintManager> hm = HintManager::GetFromJSON(config_path, false);
    if (!hm) {
        LOG(FATAL) << "Invalid config: " << config_path;
    }

    // single thread
    ABinderProcess_setThreadPoolMaxThreadCount(0);

    // core service
    std::shared_ptr<Power> pw = ndk::SharedRefBase::make<Power>(hm);
    ndk::SpAIBinder pwBinder = pw->asBinder();
    AIBinder_setMinSchedulerPolicy(pwBinder.get(), SCHED_NORMAL, -20);

    // extension service
    std::shared_ptr<PowerExt> pwExt = ndk::SharedRefBase::make<PowerExt>(hm);
    auto pwExtBinder = pwExt->asBinder();
    AIBinder_setMinSchedulerPolicy(pwExtBinder.get(), SCHED_NORMAL, -20);

    // attach the extension to the same binder we will be registering
    CHECK(STATUS_OK == AIBinder_setExtension(pwBinder.get(), pwExt->asBinder().get()));

    const std::string instance = std::string() + Power::descriptor + "/default";
    binder_status_t status = AServiceManager_addService(pw->asBinder().get(), instance.c_str());
    CHECK(status == STATUS_OK);
    LOG(INFO) << "Xiaomi Power HAL AIDL Service with Extension started.";

    if (HintManager::GetInstance()->GetAdpfProfile()) {
        PowerHintMonitor::getInstance()->start();
    }

    std::thread initThread([&]() {
        ::android::base::WaitForProperty(kPowerHalInitProp.data(), "1");
        hm->Start();
    });
    initThread.detach();

    ABinderProcess_joinThreadPool();

    // should not reach
    LOG(ERROR) << "Xiaomi Power HAL AIDL Service with Extension just died.";
    return EXIT_FAILURE;
}
