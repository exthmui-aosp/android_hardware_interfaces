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
    IMediaQualityCallback::IMediaQualityCallback,
    AmbientBacklightEvent::AmbientBacklightEvent,
    AmbientBacklightSettings::AmbientBacklightSettings,
};
use binder::Interface;
use std::sync::{Arc, Mutex};
use std::thread;
use binder::Strong;

/// Defined so we can implement the IMediaQuality AIDL interface.
pub struct MediaQualityService {
    callback: Arc<Mutex<Option<Strong<dyn IMediaQualityCallback>>>>,
    ambient_backlight_enabled: Arc<Mutex<bool>>,
    ambient_backlight_detector_settings: Arc<Mutex<AmbientBacklightSettings>>
}

impl MediaQualityService {

    /// Create a new instance of the MediaQualityService.
    pub fn new() -> Self {
        Self {
            callback: Arc::new(Mutex::new(None)),
            ambient_backlight_enabled: Arc::new(Mutex::new(true)),
            ambient_backlight_detector_settings:
                    Arc::new(Mutex::new(AmbientBacklightSettings::default())),
        }
    }
}

impl Interface for MediaQualityService {}

impl IMediaQuality for MediaQualityService {

    fn setCallback(
        &self,
        callback: &Strong<dyn IMediaQualityCallback>
    ) -> binder::Result<()> {
        println!("Received callback: {:?}", callback);
        let mut cb = self.callback.lock().unwrap();
        *cb = Some(callback.clone());
        Ok(())
    }

    fn setAmbientBacklightDetector(
        &self,
        settings: &AmbientBacklightSettings
    ) -> binder::Result<()> {
        println!("Received settings: {:?}", settings);
        let mut ambient_backlight_detector_settings = self.ambient_backlight_detector_settings.lock().unwrap();
        ambient_backlight_detector_settings.packageName = settings.packageName.clone();
        ambient_backlight_detector_settings.source = settings.source;
        ambient_backlight_detector_settings.maxFramerate = settings.maxFramerate;
        ambient_backlight_detector_settings.colorFormat = settings.colorFormat;
        ambient_backlight_detector_settings.hZonesNumber = settings.hZonesNumber;
        ambient_backlight_detector_settings.vZonesNumber = settings.vZonesNumber;
        ambient_backlight_detector_settings.hasLetterbox = settings.hasLetterbox;
        ambient_backlight_detector_settings.threshold = settings.threshold;
        Ok(())
    }

    fn setAmbientBacklightDetectionEnabled(&self, enabled: bool) -> binder::Result<()> {
        println!("Received enabled: {}", enabled);
        let mut ambient_backlight_enabled = self.ambient_backlight_enabled.lock().unwrap();
        *ambient_backlight_enabled = enabled;
        if enabled {
            println!("Enable Ambient Backlight detection");
            thread::scope(|s| {
                s.spawn(|| {
                    let cb = self.callback.lock().unwrap();
                    if let Some(cb) = &*cb {
                        let enabled_event = AmbientBacklightEvent::Enabled(true);
                        cb.notifyAmbientBacklightEvent(&enabled_event).unwrap();
                    }
                });
            });
        } else {
            println!("Disable Ambient Backlight detection");
            thread::scope(|s| {
                s.spawn(|| {
                    let cb = self.callback.lock().unwrap();
                    if let Some(cb) = &*cb {
                        let disabled_event = AmbientBacklightEvent::Enabled(false);
                        cb.notifyAmbientBacklightEvent(&disabled_event).unwrap();
                    }
                });
            });
        }
        Ok(())
    }

    fn getAmbientBacklightDetectionEnabled(&self) -> binder::Result<bool> {
        let ambient_backlight_enabled = self.ambient_backlight_enabled.lock().unwrap();
        Ok(*ambient_backlight_enabled)
    }
}
