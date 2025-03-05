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

//! VTS test library for HwCrypto functionality.
//! It provides the base clases necessaries to write HwCrypto VTS tests

#[cfg(target_arch = "x86_64")]
use anyhow::Context;
use anyhow::Result;
#[cfg(target_arch = "x86_64")]
use binder::{ExceptionCode, FromIBinder, IntoBinderResult, ParcelFileDescriptor};
#[cfg(target_arch = "x86_64")]
use rpcbinder::RpcSession;
#[cfg(target_arch = "x86_64")]
use vsock::VsockStream;
#[cfg(target_arch = "x86_64")]
use std::os::fd::{FromRawFd, IntoRawFd};
#[cfg(target_arch = "x86_64")]
use std::fs::File;
#[cfg(target_arch = "x86_64")]
use std::io::Read;
#[cfg(target_arch = "x86_64")]
use rustutils::system_properties;
#[cfg(target_arch = "aarch64")]
use android_hardware_security_see_hwcrypto::aidl::android::hardware::security::see::hwcrypto::IHwCryptoKey::BpHwCryptoKey;
use android_hardware_security_see_hwcrypto::aidl::android::hardware::security::see::hwcrypto::IHwCryptoKey::IHwCryptoKey;

#[cfg(target_arch = "x86_64")]
const HWCRYPTO_SERVICE_PORT: u32 = 4;

/// Local function to connect to service
#[cfg(target_arch = "x86_64")]
pub fn connect_service<T: FromIBinder + ?Sized>(
    cid: u32,
    port: u32,
) -> Result<binder::Strong<T>, binder::StatusCode> {
    RpcSession::new().setup_preconnected_client(|| {
        let mut stream = VsockStream::connect_with_cid_port(cid, port).ok()?;
        let mut buffer = [0];
        let _ = stream.read(&mut buffer);
        // SAFETY: ownership is transferred from stream to f
        let f = unsafe { File::from_raw_fd(stream.into_raw_fd()) };
        Some(ParcelFileDescriptor::new(f).into_raw_fd())
    })
}

/// Get a HwCryptoKey binder service object using a direct vsock connection
#[cfg(target_arch = "x86_64")]
pub fn get_hwcryptokey() -> Result<binder::Strong<dyn IHwCryptoKey>, binder::Status> {
    let cid = system_properties::read("trusty.test_vm.vm_cid")
        .context("couldn't get vm cid")
        .or_binder_exception(ExceptionCode::ILLEGAL_STATE)?
        .ok_or(ExceptionCode::ILLEGAL_STATE)?
        .parse::<u32>()
        .context("couldn't parse vm cid")
        .or_binder_exception(ExceptionCode::ILLEGAL_ARGUMENT)?;
    Ok(connect_service(cid, HWCRYPTO_SERVICE_PORT)?)
}

/// Get a HwCryptoKey binder service object using the service manager
#[cfg(target_arch = "aarch64")]
pub fn get_hwcryptokey() -> Result<binder::Strong<dyn IHwCryptoKey>, binder::Status> {
    let interface_name = <BpHwCryptoKey as IHwCryptoKey>::get_descriptor().to_owned() + "/default";
    Ok(binder::get_interface(&interface_name)?)
}
