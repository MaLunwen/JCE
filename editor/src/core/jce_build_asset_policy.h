/*
 * jce_build_asset_policy.h
 *
 * UI-free publication policy for files discovered under runtime asset roots.
 */

#ifndef JCE_BUILD_ASSET_POLICY_H
#define JCE_BUILD_ASSET_POLICY_H

#include <string_view>

bool jce_build_asset_path_is_publishable(std::string_view virtual_path);

#endif /* JCE_BUILD_ASSET_POLICY_H */
