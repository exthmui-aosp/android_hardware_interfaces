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
#pragma once

#include <memory>
#include <vector>

namespace android::hardware::radio::minimal {

class RadioSlotBase;

class SlotContext {
  public:
    SlotContext(unsigned slotIndex);

    /**
     * Mark this RIL/modem as connected. This triggers communication with the framework.
     */
    void setConnected();
    bool isConnected() const;

    unsigned getSlotIndex() const;

    void addHal(std::weak_ptr<RadioSlotBase> hal);

  private:
    bool mIsConnected = false;
    unsigned mSlotIndex;
    std::vector<std::weak_ptr<RadioSlotBase>> mHals;
};

}  // namespace android::hardware::radio::minimal
