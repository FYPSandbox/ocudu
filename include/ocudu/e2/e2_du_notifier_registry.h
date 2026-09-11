/*
 *
 * Copyright 2021-2026 Software Radio Systems Limited
 *
 * This file is part of OCUDU.
 *
 * OCUDU is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * OCUDU is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#pragma once

#include "e2_du_ue_context_notifier.h"
#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace ocudu {

/// \brief Global registry for E2 DU UE context notifiers
/// This allows the E2 agent to register notifier instances that the DU manager can access.
/// Multiple notifiers can be registered at once (e.g. E2SM-RC's Style 4 report service and
/// E2SM-KPM's DU measurement provider both need UE-context updates), so registration is a
/// fan-out list rather than a single overwritable slot.
class e2_du_notifier_registry
{
public:
  /// \brief Get the singleton instance of the registry
  static e2_du_notifier_registry& get_instance()
  {
    static e2_du_notifier_registry instance;
    return instance;
  }

  /// \brief Register a UE context notifier (typically called when an E2 service model that needs
  /// UE-context updates is constructed, e.g. RC Report Style 4 or the KPM DU measurement provider)
  /// \param[in] notifier Pointer to the notifier instance
  void register_ue_context_notifier(e2_du_ue_context_notifier* notifier)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(ue_context_notifiers_.begin(), ue_context_notifiers_.end(), notifier) ==
        ue_context_notifiers_.end()) {
      ue_context_notifiers_.push_back(notifier);
    }
  }

  /// \brief Unregister a previously registered UE context notifier (typically called on destruction)
  void unregister_ue_context_notifier(e2_du_ue_context_notifier* notifier)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find(ue_context_notifiers_.begin(), ue_context_notifiers_.end(), notifier);
    if (it != ue_context_notifiers_.end()) {
      ue_context_notifiers_.erase(it);
    }
  }

  /// \brief Get all currently registered UE context notifiers
  /// \return Copy of the list of registered notifiers (empty if none are registered)
  std::vector<e2_du_ue_context_notifier*> get_ue_context_notifiers()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return ue_context_notifiers_;
  }

private:
  e2_du_notifier_registry() = default;
  ~e2_du_notifier_registry() = default;

  // Delete copy/move constructors and assignment operators
  e2_du_notifier_registry(const e2_du_notifier_registry&) = delete;
  e2_du_notifier_registry& operator=(const e2_du_notifier_registry&) = delete;
  e2_du_notifier_registry(e2_du_notifier_registry&&) = delete;
  e2_du_notifier_registry& operator=(e2_du_notifier_registry&&) = delete;

  std::mutex                              mutex_;
  std::vector<e2_du_ue_context_notifier*> ue_context_notifiers_;
};

} // namespace ocudu
