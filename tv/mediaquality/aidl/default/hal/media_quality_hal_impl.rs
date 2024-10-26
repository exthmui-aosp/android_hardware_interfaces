/*
 * Copyright (C) 2024 The Android Open Source Project
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
//! This module implements the IMediaQuality AIDL interface.

use android_hardware_tv_mediaquality::aidl::android::hardware::tv::mediaquality::{
    IMediaQuality::IMediaQuality,
};
use binder::Interface;
use std::sync::{Arc, Mutex};
/// Defined so we can implement the IMediaQuality AIDL interface.
pub struct MediaQualityService {
    ambient_light_enabled: Arc<Mutex<bool>>
}

impl MediaQualityService {

    /// Create a new instance of the MediaQualityService.
    pub fn new() -> Self {
        Self {
            ambient_light_enabled: Arc::new(Mutex::new(false)),
        }
    }
}

impl Interface for MediaQualityService {}

impl IMediaQuality for MediaQualityService {
    fn setAmbientLightDetectionEnabled(&self, enabled: bool) -> binder::Result<()> {
        println!("Received enabled: {}", enabled);
        let mut ambient_light_enabled = self.ambient_light_enabled.lock().unwrap();
        *ambient_light_enabled = enabled;
        Ok(())
    }

    fn getAmbientLightDetectionEnabled(&self) -> binder::Result<bool> {
        let ambient_light_enabled = self.ambient_light_enabled.lock().unwrap();
        Ok(*ambient_light_enabled)
    }
}
