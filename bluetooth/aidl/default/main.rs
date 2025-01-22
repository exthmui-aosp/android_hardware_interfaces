// Copyright 2024, The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use android_hardware_bluetooth::aidl::android::hardware::bluetooth::IBluetoothHci::{
    self,
    IBluetoothHci as _
};
use android_hardware_bluetooth::binder;
use bluetooth_offload_hal::{ HciHalProxy, CInterface };
use bluetooth_offload_leaudio_hci::LeAudioModuleBuilder;
use log;
use std::panic;

fn new_hal() -> CInterface {
    extern "C" { fn hal_new() -> CInterface; }
    unsafe { hal_new() }
}

fn main() {
    logger::init(
        logger::Config::default()
            .with_max_level(log::LevelFilter::Debug)
            .with_tag_on_device("android.hardware.bluetooth"),
    );

    panic::set_hook(Box::new(|panic_info| {
        log::error!("{}", panic_info);
    }));

    log::info!("Bluetooth HAL starting up");

    binder::ProcessState::set_thread_pool_max_thread_count(0);
    binder::ProcessState::start_thread_pool();

    binder::add_service(
        &format!("{}/default", IBluetoothHci::BpBluetoothHci::get_descriptor()),
        IBluetoothHci::BnBluetoothHci::new_binder(
            HciHalProxy::new(
                vec![ Box::new(LeAudioModuleBuilder {}) ],
                new_hal()
            ),
            binder::BinderFeatures::default(),
        ).as_binder()
    ).expect("Failed to register service");

    binder::ProcessState::join_thread_pool();
}
