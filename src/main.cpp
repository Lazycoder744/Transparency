#include "main.hpp"

#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML-Lite/Creation/Layout.hpp"
#include "bsml/shared/BSML-Lite/Creation/Settings.hpp"
#include "bsml/shared/BSML/Components/Settings/ToggleSetting.hpp"
#include "GlobalNamespace/BaseNoteVisuals.hpp"
#include "GlobalNamespace/BombExplosionEffect.hpp"
#include "GlobalNamespace/BombNoteController.hpp"
#include "GlobalNamespace/BoxCuttableBySaber.hpp"
#include "GlobalNamespace/BurstSliderGameNoteController.hpp"
#include "GlobalNamespace/ColorNoteVisuals.hpp"
#include "GlobalNamespace/CuttableBySaber.hpp"
#include "GlobalNamespace/EnvironmentSceneSetup.hpp"
#include "GlobalNamespace/GameNoteController.hpp"
#include "GlobalNamespace/GameplayCoreInstaller.hpp"
#include "GlobalNamespace/NoteControllerBase.hpp"
#include "GlobalNamespace/NoteCutParticlesEffect.hpp"
#include "GlobalNamespace/NoteDebris.hpp"
#include "GlobalNamespace/NoteTrailEffect.hpp"
#include "GlobalNamespace/OVRCameraRig.hpp"
#include "GlobalNamespace/OVRManager.hpp"
#include "GlobalNamespace/OVROverlay.hpp"
#include "GlobalNamespace/OVRPassthroughLayer.hpp"
#include "GlobalNamespace/OVRPlugin.hpp"
#include "GlobalNamespace/OVRPose.hpp"
#include "GlobalNamespace/ObstacleController.hpp"
#include "GlobalNamespace/ObstacleControllerBase.hpp"
#include "GlobalNamespace/Saber.hpp"
#include "GlobalNamespace/SaberClashEffect.hpp"
#include "GlobalNamespace/SaberModelController.hpp"
#include "GlobalNamespace/SaberTrail.hpp"
#include "GlobalNamespace/SaberTrailRenderer.hpp"
#include "GlobalNamespace/SliderController.hpp"
#include "GlobalNamespace/SliderControllerBase.hpp"
#include "GlobalNamespace/SliderMeshController.hpp"
#include "GlobalNamespace/SphereCuttableBySaber.hpp"
#include "GlobalNamespace/StandardLevelGameplayManager.hpp"
#include "GlobalNamespace/StretchableObstacle.hpp"
#include "scotland2/shared/modloader.h"
#include "UnityEngine/Camera.hpp"
#include "UnityEngine/CameraClearFlags.hpp"
#include "UnityEngine/Behaviour.hpp"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/Component.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Object.hpp"
#include "UnityEngine/Renderer.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/Vector3.hpp"

#include <android/log.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <string>
#include <string_view>
#include <vector>

static modloader::ModInfo modInfo{MOD_ID, VERSION, 0};
// Stores the ID and version of our mod, and is sent to
// the modloader upon startup

namespace {
constexpr std::string_view kEnabledConfigKey = "enabled";
constexpr std::string_view kOpacityConfigKey = "opacity";
constexpr std::string_view kBrightnessConfigKey = "brightness";
constexpr std::string_view kContrastConfigKey = "contrast";
constexpr std::string_view kSaturationConfigKey = "saturation";
constexpr std::string_view kPassthroughObjectName = "TransparencyPassthroughLayer";
constexpr float kDefaultPassthroughOpacity = 0.30f;
constexpr float kDefaultPassthroughBrightness = -0.20f;
constexpr float kDefaultPassthroughContrast = 0.45f;
constexpr float kDefaultPassthroughSaturation = -0.50f;

struct CameraState {
  ::UnityW<::UnityEngine::Camera> camera;
  ::UnityEngine::CameraClearFlags clearFlags;
  ::UnityEngine::Color backgroundColor;
};

bool passthroughEnabled = true;
bool inLevel = false;
bool bsmlRegistered = false;
int sceneRefreshPasses = 0;
bool passthroughUnsupportedLogged = false;
bool passthroughInitRequestedLogged = false;
bool passthroughPendingLogged = false;
bool passthroughInitializedLogged = false;
bool passthroughFailedLogged = false;
bool ovrManagerPassthroughRequested = false;
bool ovrManagerStateCaptured = false;
bool originalOvrManagerPassthroughEnabled = false;
bool eyeFovAlphaModeStateCaptured = false;
bool originalEyeFovAlphaModeEnabled = false;
bool ovrManagerTargetLogged = false;
bool lastOvrManagerTarget = false;
bool lastEyeFovAlphaTarget = false;
bool passthroughLayerLifecycleForced = false;
int passthroughLayerStateLogsRemaining = 8;
bool ovrManagerLookupLogged = false;
bool cameraRigLookupLogged = false;
int directOverlayCreateAttemptsRemaining = 24;
int overlayCreateHookLogsRemaining = 8;
int overlaySubmitHookLogsRemaining = 24;
int directOverlaySubmitFrameIndex = 0;
int directOverlaySubmitLogsRemaining = 60;
bool passthroughStyleLogged = false;
bool passthroughBcsStyleFailedLogged = false;
float passthroughOpacity = kDefaultPassthroughOpacity;
float passthroughBrightness = kDefaultPassthroughBrightness;
float passthroughContrast = kDefaultPassthroughContrast;
float passthroughSaturation = kDefaultPassthroughSaturation;

::UnityW<::GlobalNamespace::OVRPassthroughLayer> passthroughLayer;
std::vector<CameraState> changedCameras;
std::vector<::UnityW<::UnityEngine::Renderer>> hiddenRenderers;

bool EffectShouldRun() { return passthroughEnabled && inLevel; }

void AndroidInfo(char const* message) { __android_log_print(ANDROID_LOG_INFO, "Transparency", "%s", message); }

void ApplyPassthroughVisualStyle() {
  if (!passthroughLayer) {
    return;
  }

  passthroughLayer->set_textureOpacity(passthroughOpacity);

  try {
    passthroughLayer->SetBrightnessContrastSaturation(passthroughBrightness, passthroughContrast, passthroughSaturation);
  } catch (...) {
    if (!passthroughBcsStyleFailedLogged) {
      passthroughBcsStyleFailedLogged = true;
      __android_log_print(ANDROID_LOG_INFO, "Transparency", "Passthrough BCS style threw; using opacity-only style.");
      PaperLogger.warn("Passthrough BCS style threw; using opacity-only style.");
    }
  }

  passthroughLayer->SetStyleDirty();

  if (!passthroughStyleLogged) {
    passthroughStyleLogged = true;
    __android_log_print(ANDROID_LOG_INFO, "Transparency", "Passthrough visual style opacity=%.2f brightness=%.2f contrast=%.2f saturation=%.2f", passthroughOpacity,
                        passthroughBrightness, passthroughContrast, passthroughSaturation);
    PaperLogger.info("Passthrough visual style opacity={}, brightness={}, contrast={}, saturation={}", passthroughOpacity, passthroughBrightness, passthroughContrast,
                     passthroughSaturation);
  }
}

void LogPassthroughRuntimeState(char const* context) {
  auto state = ::GlobalNamespace::OVRPlugin::GetInsightPassthroughInitializationState();
  bool supported = ::GlobalNamespace::OVRManager::IsInsightPassthroughSupported();
  bool initialized = ::GlobalNamespace::OVRManager::IsInsightPassthroughInitialized();
  bool pending = ::GlobalNamespace::OVRManager::IsInsightPassthroughInitPending();
  bool failed = ::GlobalNamespace::OVRManager::HasInsightPassthroughInitFailed();
  int32_t stateValue = static_cast<int32_t>(state);

  __android_log_print(ANDROID_LOG_INFO, "Transparency", "%s: supported=%d initialized=%d pending=%d failed=%d state=%d", context, supported, initialized, pending, failed,
                      stateValue);
  PaperLogger.info("{}: supported={}, initialized={}, pending={}, failed={}, state={}", context, supported, initialized, pending, failed, stateValue);
}

void LogPassthroughLayerState(char const* context, bool force = false) {
  if (!force) {
    if (passthroughLayerStateLogsRemaining <= 0) {
      return;
    }
    passthroughLayerStateLogsRemaining--;
  }

  if (!passthroughLayer) {
    __android_log_print(ANDROID_LOG_INFO, "Transparency", "%s: passthroughLayer=null", context);
    PaperLogger.info("{}: passthroughLayer=null", context);
    return;
  }

  auto layerObject = passthroughLayer->get_gameObject();
  bool layerEnabled = passthroughLayer->get_enabled();
  bool layerActive = layerObject && layerObject->get_activeInHierarchy();
  bool layerActiveSelf = layerObject && layerObject->get_activeSelf();
  bool layerHidden = passthroughLayer->__cordl_internal_get_hidden();
  bool cameraRigInitialized = passthroughLayer->__cordl_internal_get_cameraRigInitialized();
  bool hasCameraRig = passthroughLayer->__cordl_internal_get_cameraRig();

  auto overlay = passthroughLayer->__cordl_internal_get_passthroughOverlay();
  bool hasOverlay = overlay;
  bool overlayEnabled = false;
  bool overlayActive = false;
  bool overlayHidden = false;
  int32_t overlayLayerId = -1;
  int32_t overlayType = -1;
  int32_t overlayShape = -1;
  int32_t overlayCurrentShape = -1;
  int32_t overlayCompositionDepth = 0;

  if (overlay) {
    auto overlayObject = overlay->get_gameObject();
    overlayEnabled = overlay->get_enabled();
    overlayActive = overlayObject && overlayObject->get_activeInHierarchy();
    overlayHidden = overlay->__cordl_internal_get_hidden();
    overlayLayerId = overlay->get_layerId();
    overlayType = static_cast<int32_t>(overlay->__cordl_internal_get_currentOverlayType());
    overlayShape = static_cast<int32_t>(passthroughLayer->get_overlayShape());
    overlayCurrentShape = static_cast<int32_t>(overlay->__cordl_internal_get_currentOverlayShape());
    overlayCompositionDepth = overlay->__cordl_internal_get_compositionDepth();
  }

  __android_log_print(ANDROID_LOG_INFO, "Transparency",
                      "%s: layer enabled=%d active=%d activeSelf=%d hidden=%d cameraRig=%d cameraRigInit=%d overlay=%d overlayEnabled=%d overlayActive=%d overlayHidden=%d overlayId=%d "
                      "overlayType=%d overlayShape=%d overlayCurrentShape=%d overlayDepth=%d",
                      context, layerEnabled, layerActive, layerActiveSelf, layerHidden, hasCameraRig, cameraRigInitialized, hasOverlay, overlayEnabled, overlayActive, overlayHidden,
                      overlayLayerId, overlayType, overlayShape, overlayCurrentShape, overlayCompositionDepth);
  PaperLogger.info(
      "{}: layer enabled={}, active={}, activeSelf={}, hidden={}, cameraRig={}, cameraRigInit={}, overlay={}, overlayEnabled={}, overlayActive={}, overlayHidden={}, overlayId={}, overlayType={}, "
      "overlayShape={}, overlayCurrentShape={}, overlayDepth={}",
      context, layerEnabled, layerActive, layerActiveSelf, layerHidden, hasCameraRig, cameraRigInitialized, hasOverlay, overlayEnabled, overlayActive, overlayHidden, overlayLayerId, overlayType,
      overlayShape, overlayCurrentShape, overlayCompositionDepth);
}

::UnityW<::GlobalNamespace::OVRManager> FindOvrManager() {
  auto manager = ::GlobalNamespace::OVRManager::get_instance();
  if (manager) {
    return manager;
  }

  manager = ::UnityEngine::Object::FindObjectOfType<::GlobalNamespace::OVRManager*>(true);
  if (manager) {
    ::GlobalNamespace::OVRManager::set_instance(manager);
  }

  if (!ovrManagerLookupLogged || manager) {
    ovrManagerLookupLogged = true;
    __android_log_print(ANDROID_LOG_INFO, "Transparency", "OVRManager lookup fallback manager=%d", static_cast<bool>(manager));
    PaperLogger.info("OVRManager lookup fallback manager={}", static_cast<bool>(manager));
  }

  return manager;
}

::UnityW<::GlobalNamespace::OVRCameraRig> FindOvrCameraRig() {
  auto rig = ::UnityEngine::Object::FindObjectOfType<::GlobalNamespace::OVRCameraRig*>(true);
  if (!cameraRigLookupLogged || rig) {
    cameraRigLookupLogged = true;
    __android_log_print(ANDROID_LOG_INFO, "Transparency", "OVRCameraRig lookup rig=%d", static_cast<bool>(rig));
    PaperLogger.info("OVRCameraRig lookup rig={}", static_cast<bool>(rig));
  }

  return rig;
}

void AttachCameraRigIfAvailable() {
  if (!passthroughLayer || passthroughLayer->__cordl_internal_get_cameraRig()) {
    return;
  }

  auto rig = FindOvrCameraRig();
  if (!rig) {
    return;
  }

  passthroughLayer->__cordl_internal_set_cameraRig(rig);
  passthroughLayer->__cordl_internal_set_cameraRigInitialized(true);
  AndroidInfo("Attached OVRCameraRig to passthrough layer.");
  PaperLogger.info("Attached OVRCameraRig to passthrough layer.");
}

::UnityW<::GlobalNamespace::OVROverlay> GetPassthroughOverlay() {
  if (!passthroughLayer) {
    return nullptr;
  }

  return passthroughLayer->__cordl_internal_get_passthroughOverlay();
}

bool IsTransparencyOverlay(::GlobalNamespace::OVROverlay* overlay) {
  auto currentOverlay = GetPassthroughOverlay();
  return overlay && currentOverlay && overlay == static_cast<::GlobalNamespace::OVROverlay*>(currentOverlay);
}

void TryCreateOverlayLayerDirectly(char const* context) {
  if (directOverlayCreateAttemptsRemaining <= 0) {
    return;
  }

  if (!passthroughLayer) {
    directOverlayCreateAttemptsRemaining--;
    __android_log_print(ANDROID_LOG_INFO, "Transparency", "direct OVROverlay CreateLayer skipped from %s: passthroughLayer=null attemptsLeft=%d", context,
                        directOverlayCreateAttemptsRemaining);
    return;
  }

  auto overlay = GetPassthroughOverlay();
  if (!overlay) {
    directOverlayCreateAttemptsRemaining--;
    __android_log_print(ANDROID_LOG_INFO, "Transparency", "direct OVROverlay CreateLayer skipped from %s: overlay=null attemptsLeft=%d", context, directOverlayCreateAttemptsRemaining);
    return;
  }

  int32_t beforeLayerId = overlay->get_layerId();
  if (beforeLayerId != 0) {
    directOverlayCreateAttemptsRemaining = 0;
    __android_log_print(ANDROID_LOG_INFO, "Transparency", "direct OVROverlay CreateLayer skipped from %s: existingLayerId=%d", context, beforeLayerId);
    return;
  }

  directOverlayCreateAttemptsRemaining--;
  auto shape = ::GlobalNamespace::OVRPlugin_OverlayShape::ReconstructionPassthrough;
  auto size = ::GlobalNamespace::OVRPlugin_Sizei(0, 0);
  int32_t flags = static_cast<int32_t>(::GlobalNamespace::OVRPlugin_LayerFlags::NoAllocation);

  try {
    overlay->InitOVROverlay();
    bool created = overlay->CreateLayer(1, 1, ::GlobalNamespace::OVRPlugin_EyeTextureFormat::Default, flags, size, shape);
    int32_t afterLayerId = overlay->get_layerId();

    __android_log_print(ANDROID_LOG_INFO, "Transparency", "direct OVROverlay CreateLayer from %s result=%d beforeId=%d afterId=%d flags=%d shape=%d attemptsLeft=%d", context,
                        created, beforeLayerId, afterLayerId, flags, static_cast<int32_t>(shape), directOverlayCreateAttemptsRemaining);
    PaperLogger.info("direct OVROverlay CreateLayer from {} result={}, beforeId={}, afterId={}, flags={}, shape={}, attemptsLeft={}", context, created, beforeLayerId, afterLayerId,
                     flags, static_cast<int32_t>(shape), directOverlayCreateAttemptsRemaining);

    overlay->set_enabled(true);
    overlay->__cordl_internal_set_hidden(false);
    overlay->LateUpdate();
  } catch (...) {
    __android_log_print(ANDROID_LOG_INFO, "Transparency", "direct OVROverlay CreateLayer from %s threw; attemptsLeft=%d", context, directOverlayCreateAttemptsRemaining);
    PaperLogger.warn("direct OVROverlay CreateLayer from {} threw; attemptsLeft={}", context, directOverlayCreateAttemptsRemaining);
  }
}

void SubmitOverlayLayerDirectly(char const* context) {
  if (!EffectShouldRun() || !passthroughLayer) {
    return;
  }

  auto overlay = GetPassthroughOverlay();
  if (!overlay) {
    return;
  }

  int32_t layerId = overlay->get_layerId();
  if (layerId == 0) {
    return;
  }

  try {
    overlay->set_enabled(true);
    overlay->__cordl_internal_set_hidden(false);

    auto pose = ::GlobalNamespace::OVRPose::get_identity();
    auto scale = ::UnityEngine::Vector3(1.0f, 1.0f, 1.0f);
    bool noDepthBufferTesting = overlay->__cordl_internal_get_noDepthBufferTesting();
    int32_t frameIndex = directOverlaySubmitFrameIndex++;
    bool submitted = overlay->SubmitLayer(false, false, noDepthBufferTesting, pose, scale, frameIndex);

    if (directOverlaySubmitLogsRemaining > 0) {
      directOverlaySubmitLogsRemaining--;
      __android_log_print(ANDROID_LOG_INFO, "Transparency",
                          "direct OVROverlay SubmitLayer from %s result=%d layerId=%d noDepth=%d frame=%d logsLeft=%d", context, submitted, layerId,
                          noDepthBufferTesting, frameIndex, directOverlaySubmitLogsRemaining);
      PaperLogger.info("direct OVROverlay SubmitLayer from {} result={}, layerId={}, noDepth={}, frame={}, logsLeft={}", context, submitted, layerId, noDepthBufferTesting,
                       frameIndex, directOverlaySubmitLogsRemaining);
    }
  } catch (...) {
    if (directOverlaySubmitLogsRemaining > 0) {
      directOverlaySubmitLogsRemaining--;
      __android_log_print(ANDROID_LOG_INFO, "Transparency", "direct OVROverlay SubmitLayer from %s threw; layerId=%d logsLeft=%d", context, layerId,
                          directOverlaySubmitLogsRemaining);
      PaperLogger.warn("direct OVROverlay SubmitLayer from {} threw; layerId={}, logsLeft={}", context, layerId, directOverlaySubmitLogsRemaining);
    }
  }
}

std::string ToStdString(::StringW value) { return static_cast<std::string>(value); }

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

bool ContainsAny(std::string_view value, std::span<const std::string_view> needles) {
  for (auto needle : needles) {
    if (!needle.empty() && value.find(needle) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

std::string ObjectName(::UnityEngine::Object* object) {
  if (!object) {
    return {};
  }
  return ToStdString(object->get_name());
}

std::string HierarchyName(::UnityEngine::Component* component) {
  if (!component) {
    return {};
  }

  std::string hierarchy;
  auto transform = component->get_transform();
  for (int depth = 0; transform && depth < 32; depth++) {
    if (!hierarchy.empty()) {
      hierarchy += '/';
    }
    hierarchy += ObjectName(transform);
    transform = transform->get_parent();
  }
  return hierarchy;
}

bool ReadEnabledConfig() {
  auto& document = getConfig().config;
  if (document.IsObject() && document.HasMember(kEnabledConfigKey.data()) && document[kEnabledConfigKey.data()].IsBool()) {
    return document[kEnabledConfigKey.data()].GetBool();
  }
  return true;
}

float ReadFloatConfig(std::string_view key, float fallback, float minValue, float maxValue) {
  auto& document = getConfig().config;
  if (document.IsObject() && document.HasMember(key.data()) && document[key.data()].IsNumber()) {
    return std::clamp(document[key.data()].GetFloat(), minValue, maxValue);
  }
  return fallback;
}

void WriteEnabledConfig(bool value) {
  auto& configuration = getConfig();
  auto& document = configuration.config;
  if (!document.IsObject()) {
    document.SetObject();
  }

  auto& allocator = document.GetAllocator();
  if (document.HasMember(kEnabledConfigKey.data())) {
    document[kEnabledConfigKey.data()].SetBool(value);
  } else {
    document.AddMember(::rapidjson::Value(kEnabledConfigKey.data(), allocator), ::rapidjson::Value(value), allocator);
  }

  configuration.Write();
}

void WriteFloatConfig(std::string_view key, float value) {
  auto& configuration = getConfig();
  auto& document = configuration.config;
  if (!document.IsObject()) {
    document.SetObject();
  }

  auto& allocator = document.GetAllocator();
  if (document.HasMember(key.data())) {
    document[key.data()].SetFloat(value);
  } else {
    document.AddMember(::rapidjson::Value(key.data(), allocator), ::rapidjson::Value(value), allocator);
  }

  configuration.Write();
}

template <typename T> bool HasComponentInParent(::UnityEngine::GameObject* gameObject) {
  return gameObject && gameObject->GetComponentInParent<T>(true);
}

bool HasGameplayComponentParent(::UnityEngine::Renderer* renderer) {
  if (!renderer) {
    return false;
  }

  auto gameObject = renderer->get_gameObject();
  if (!gameObject) {
    return false;
  }

  return HasComponentInParent<::GlobalNamespace::Saber*>(gameObject) || HasComponentInParent<::GlobalNamespace::SaberModelController*>(gameObject) ||
         HasComponentInParent<::GlobalNamespace::SaberTrail*>(gameObject) || HasComponentInParent<::GlobalNamespace::SaberTrailRenderer*>(gameObject) ||
         HasComponentInParent<::GlobalNamespace::SaberClashEffect*>(gameObject) || HasComponentInParent<::GlobalNamespace::NoteControllerBase*>(gameObject) ||
         HasComponentInParent<::GlobalNamespace::GameNoteController*>(gameObject) || HasComponentInParent<::GlobalNamespace::BombNoteController*>(gameObject) ||
         HasComponentInParent<::GlobalNamespace::BombExplosionEffect*>(gameObject) || HasComponentInParent<::GlobalNamespace::BaseNoteVisuals*>(gameObject) ||
         HasComponentInParent<::GlobalNamespace::ColorNoteVisuals*>(gameObject) || HasComponentInParent<::GlobalNamespace::BurstSliderGameNoteController*>(gameObject) ||
         HasComponentInParent<::GlobalNamespace::SliderControllerBase*>(gameObject) || HasComponentInParent<::GlobalNamespace::SliderController*>(gameObject) ||
         HasComponentInParent<::GlobalNamespace::SliderMeshController*>(gameObject) || HasComponentInParent<::GlobalNamespace::ObstacleControllerBase*>(gameObject) ||
         HasComponentInParent<::GlobalNamespace::ObstacleController*>(gameObject) || HasComponentInParent<::GlobalNamespace::StretchableObstacle*>(gameObject) ||
         HasComponentInParent<::GlobalNamespace::NoteDebris*>(gameObject) || HasComponentInParent<::GlobalNamespace::NoteCutParticlesEffect*>(gameObject) ||
         HasComponentInParent<::GlobalNamespace::NoteTrailEffect*>(gameObject) || HasComponentInParent<::GlobalNamespace::CuttableBySaber*>(gameObject) ||
         HasComponentInParent<::GlobalNamespace::BoxCuttableBySaber*>(gameObject) || HasComponentInParent<::GlobalNamespace::SphereCuttableBySaber*>(gameObject);
}

bool ShouldHideRenderer(::UnityEngine::Renderer* renderer) {
  if (!renderer || !renderer->get_enabled() || HasGameplayComponentParent(renderer)) {
    return false;
  }

  static constexpr std::array<std::string_view, 20> keepTokens = {"saber",  "note",  "bomb", "obstacle", "block", "debris", "slice",
                                                                  "score",  "combo", "energy", "ui",       "canvas", "text",  "hud",
                                                                  "arrow",  "pause", "menu", "multiplier", "progress", "player"};

  auto name = ToLower(HierarchyName(renderer));
  return !ContainsAny(name, keepTokens);
}

bool IsTrackedCamera(::UnityEngine::Camera* camera) {
  for (auto& state : changedCameras) {
    if (state.camera && static_cast<::UnityEngine::Camera*>(state.camera) == camera) {
      return true;
    }
  }
  return false;
}

void MakeCameraClearsTransparent() {
  auto cameras = ::UnityEngine::Camera::get_allCameras();
  for (auto camera : cameras) {
    if (!camera) {
      continue;
    }

    auto* rawCamera = static_cast<::UnityEngine::Camera*>(camera);
    if (!IsTrackedCamera(rawCamera)) {
      changedCameras.push_back({camera, camera->get_clearFlags(), camera->get_backgroundColor()});
    }

    camera->set_clearFlags(::UnityEngine::CameraClearFlags::SolidColor);
    camera->set_backgroundColor({0.0f, 0.0f, 0.0f, 0.0f});
  }
}

void RestoreCameraClears() {
  for (auto& state : changedCameras) {
    if (!state.camera) {
      continue;
    }

    state.camera->set_clearFlags(state.clearFlags);
    state.camera->set_backgroundColor(state.backgroundColor);
  }
  changedCameras.clear();
}

void HideEnvironmentRenderers() {
  auto renderers = ::UnityEngine::Object::FindObjectsOfType<::UnityEngine::Renderer*>(true);
  int hiddenThisPass = 0;

  for (auto renderer : renderers) {
    if (!ShouldHideRenderer(renderer)) {
      continue;
    }

    renderer->set_enabled(false);
    hiddenRenderers.push_back(renderer);
    hiddenThisPass++;
  }

  if (hiddenThisPass > 0) {
    PaperLogger.info("Disabled {} non-gameplay renderers for passthrough.", hiddenThisPass);
  }
}

void RestoreHiddenRenderers() {
  for (auto renderer : hiddenRenderers) {
    if (renderer) {
      renderer->set_enabled(true);
    }
  }
  hiddenRenderers.clear();
}

void SetOvrManagerPassthroughEnabled(bool enabled) {
  if (!enabled && !ovrManagerPassthroughRequested) {
    return;
  }

  bool targetState = enabled;
  bool eyeFovAlphaTarget = enabled;
  auto manager = FindOvrManager();
  bool hasManager = manager;
  if (manager) {
    if (enabled && !ovrManagerStateCaptured) {
      originalOvrManagerPassthroughEnabled = manager->__cordl_internal_get_isInsightPassthroughEnabled();
      ovrManagerStateCaptured = true;
    } else if (!enabled && ovrManagerStateCaptured) {
      targetState = originalOvrManagerPassthroughEnabled;
    }

    manager->__cordl_internal_set_isInsightPassthroughEnabled(targetState);
  }

  if (enabled && !eyeFovAlphaModeStateCaptured) {
    originalEyeFovAlphaModeEnabled = ::GlobalNamespace::OVRManager::get_eyeFovPremultipliedAlphaModeEnabled();
    eyeFovAlphaModeStateCaptured = true;
  } else if (!enabled && eyeFovAlphaModeStateCaptured) {
    eyeFovAlphaTarget = originalEyeFovAlphaModeEnabled;
  }

  ::GlobalNamespace::OVRManager::set_eyeFovPremultipliedAlphaModeEnabled(eyeFovAlphaTarget);
  ::GlobalNamespace::OVRManager::UpdateInsightPassthrough(targetState);
  ovrManagerPassthroughRequested = enabled;

  if (!ovrManagerTargetLogged || lastOvrManagerTarget != targetState || lastEyeFovAlphaTarget != eyeFovAlphaTarget) {
    ovrManagerTargetLogged = true;
    lastOvrManagerTarget = targetState;
    lastEyeFovAlphaTarget = eyeFovAlphaTarget;
    __android_log_print(ANDROID_LOG_INFO, "Transparency", "OVRManager passthrough target=%d alphaTarget=%d manager=%d original=%d originalAlpha=%d", targetState,
                        eyeFovAlphaTarget, hasManager, originalOvrManagerPassthroughEnabled, originalEyeFovAlphaModeEnabled);
    PaperLogger.info("OVRManager passthrough target={}, alphaTarget={}, manager={}, original={}, originalAlpha={}", targetState, eyeFovAlphaTarget, hasManager,
                     originalOvrManagerPassthroughEnabled, originalEyeFovAlphaModeEnabled);
  }

  if (!enabled) {
    ovrManagerStateCaptured = false;
    eyeFovAlphaModeStateCaptured = false;
  }
}

void SetPassthroughVisible(bool visible) {
  SetOvrManagerPassthroughEnabled(visible);

  if (!passthroughLayer) {
    return;
  }

  passthroughLayer->__cordl_internal_set_hidden(!visible);
  auto gameObject = passthroughLayer->get_gameObject();
  if (gameObject) {
    gameObject->SetActive(visible);
  }

  if (visible) {
    passthroughLayer->set_enabled(true);
    AttachCameraRigIfAvailable();
    ApplyPassthroughVisualStyle();
    passthroughLayer->SyncToOverlay();
    passthroughLayer->LateUpdate();

    auto overlay = passthroughLayer->__cordl_internal_get_passthroughOverlay();
    if (overlay) {
      overlay->set_enabled(true);
      overlay->__cordl_internal_set_hidden(false);
      overlay->LateUpdate();
    }

    TryCreateOverlayLayerDirectly("SetPassthroughVisible");
    SubmitOverlayLayerDirectly("SetPassthroughVisible");
  }
}

void ForcePassthroughLayerLifecycleOnce() {
  if (!passthroughLayer || passthroughLayerLifecycleForced) {
    return;
  }

  passthroughLayerLifecycleForced = true;
  LogPassthroughLayerState("before forced passthrough lifecycle", true);

  auto gameObject = passthroughLayer->get_gameObject();
  if (gameObject) {
    gameObject->SetActive(true);
  }

  passthroughLayer->set_enabled(true);
  AttachCameraRigIfAvailable();
  if (!passthroughLayer->__cordl_internal_get_passthroughOverlay()) {
    passthroughLayer->Awake();
  }

  passthroughLayer->OnEnable();
  ApplyPassthroughVisualStyle();
  passthroughLayer->SyncToOverlay();
  passthroughLayer->LateUpdate();

  auto overlay = passthroughLayer->__cordl_internal_get_passthroughOverlay();
  if (overlay) {
    overlay->set_enabled(true);
    overlay->__cordl_internal_set_hidden(false);
    overlay->OnEnable();
    overlay->LateUpdate();
  }

  LogPassthroughLayerState("after forced passthrough lifecycle", true);
}

bool EnsurePassthroughLayer() {
  if (passthroughLayer) {
    SetPassthroughVisible(true);
    return true;
  }

  if (!::GlobalNamespace::OVRManager::IsInsightPassthroughSupported()) {
    if (!passthroughUnsupportedLogged) {
      passthroughUnsupportedLogged = true;
      LogPassthroughRuntimeState("OVR insight passthrough unsupported");
    }
    return false;
  }

  SetOvrManagerPassthroughEnabled(true);

  if (::GlobalNamespace::OVRManager::HasInsightPassthroughInitFailed()) {
    if (!passthroughFailedLogged) {
      passthroughFailedLogged = true;
      LogPassthroughRuntimeState("OVR insight passthrough init failed");
    }
    return false;
  }

  if (!::GlobalNamespace::OVRManager::IsInsightPassthroughInitialized()) {
    if (!::GlobalNamespace::OVRManager::IsInsightPassthroughInitPending()) {
      bool initRequested = ::GlobalNamespace::OVRManager::InitializeInsightPassthrough();
      if (!initRequested) {
        if (!passthroughFailedLogged) {
          passthroughFailedLogged = true;
          LogPassthroughRuntimeState("OVR insight passthrough init request failed");
        }
        return false;
      }

      if (!passthroughInitRequestedLogged) {
        passthroughInitRequestedLogged = true;
        LogPassthroughRuntimeState("OVR insight passthrough init requested");
      }
    } else if (!passthroughPendingLogged) {
      passthroughPendingLogged = true;
      LogPassthroughRuntimeState("OVR insight passthrough init pending");
    }

    sceneRefreshPasses = std::max(sceneRefreshPasses, 180);
    return false;
  }

  if (!passthroughInitializedLogged) {
    passthroughInitializedLogged = true;
    LogPassthroughRuntimeState("OVR insight passthrough initialized");
  }

  auto gameObject = ::UnityEngine::GameObject::New_ctor(::StringW(kPassthroughObjectName.data()));
  if (!gameObject) {
    PaperLogger.warn("Failed to create passthrough layer GameObject.");
    return false;
  }

  gameObject->SetActive(false);
  ::UnityEngine::Object::DontDestroyOnLoad(gameObject);

  auto layer = gameObject->AddComponent<::GlobalNamespace::OVRPassthroughLayer*>();
  if (!layer) {
    PaperLogger.warn("Failed to add OVRPassthroughLayer.");
    ::UnityEngine::Object::Destroy(gameObject);
    return false;
  }

  layer->__cordl_internal_set_projectionSurfaceType(::GlobalNamespace::OVRPassthroughLayer_ProjectionSurfaceType::Reconstructed);
  layer->__cordl_internal_set_overlayType(::GlobalNamespace::OVROverlay_OverlayType::Underlay);
  layer->__cordl_internal_set_compositionDepth(-100);
  layer->__cordl_internal_set_hidden(false);
  layer->set_edgeRenderingEnabled(false);
  passthroughLayer = layer;
  ApplyPassthroughVisualStyle();

  gameObject->SetActive(true);
  LogPassthroughLayerState("after OVRPassthroughLayer SetActive", true);
  ForcePassthroughLayerLifecycleOnce();
  SetPassthroughVisible(true);
  TryCreateOverlayLayerDirectly("EnsurePassthroughLayer");
  LogPassthroughLayerState("after OVRPassthroughLayer visible", true);
  AndroidInfo("Created OVR passthrough underlay.");
  PaperLogger.info("Created OVR passthrough underlay.");
  return true;
}

void ApplyEffectIfNeeded() {
  if (!EffectShouldRun()) {
    return;
  }

  if (!EnsurePassthroughLayer()) {
    return;
  }

  MakeCameraClearsTransparent();
  HideEnvironmentRenderers();
  LogPassthroughLayerState("after applying passthrough effect");
  TryCreateOverlayLayerDirectly("ApplyEffectIfNeeded");
  SubmitOverlayLayerDirectly("ApplyEffectIfNeeded");
}

void DisableEffect() {
  SetPassthroughVisible(false);
  RestoreCameraClears();
  RestoreHiddenRenderers();
  sceneRefreshPasses = 0;
}

void EnterLevel() {
  inLevel = true;
  sceneRefreshPasses = 12;
  passthroughLayerStateLogsRemaining = 8;
  directOverlayCreateAttemptsRemaining = 24;
  overlayCreateHookLogsRemaining = 8;
  overlaySubmitHookLogsRemaining = 24;
  directOverlaySubmitFrameIndex = 0;
  directOverlaySubmitLogsRemaining = 60;
  ApplyEffectIfNeeded();
}

void ExitLevel() {
  DisableEffect();
  inLevel = false;
}

void RegisterGameplaySetupToggle() {
  if (bsmlRegistered) {
    return;
  }

  try {
    BSML::Init();
    bsmlRegistered = BSML::Register::RegisterGameplaySetupTab(
        "Transparency",
        [](UnityEngine::GameObject* parent, bool firstActivation) {
          if (!parent || !firstActivation) {
            return;
          }

          auto container = BSML::Lite::CreateScrollableSettingsContainer(parent->get_transform());
          auto settingsParent = container ? container->get_transform() : parent->get_transform();

          auto applySliderChange = []() {
            passthroughStyleLogged = false;
            if (EffectShouldRun()) {
              sceneRefreshPasses = 12;
              ApplyEffectIfNeeded();
            }
          };

          auto toggle = BSML::Lite::CreateToggle(settingsParent, "Passthrough Environment", passthroughEnabled, [](bool value) {
            passthroughEnabled = value;
            WriteEnabledConfig(value);
            if (EffectShouldRun()) {
              sceneRefreshPasses = 12;
              ApplyEffectIfNeeded();
            } else {
              DisableEffect();
            }
          });

          if (toggle) {
            toggle->get_gameObject()->set_name("TransparencyPassthroughToggle");
          }

          BSML::Lite::CreateSliderSetting(settingsParent, "Camera Opacity", 0.01f, passthroughOpacity, 0.0f, 1.0f, 0.05f, true, {0.0f, 0.0f}, [applySliderChange](float value) {
            passthroughOpacity = std::clamp(value, 0.0f, 1.0f);
            WriteFloatConfig(kOpacityConfigKey, passthroughOpacity);
            applySliderChange();
          });

          BSML::Lite::CreateSliderSetting(settingsParent, "Brightness", 0.01f, passthroughBrightness, -1.0f, 1.0f, 0.05f, true, {0.0f, 0.0f}, [applySliderChange](float value) {
            passthroughBrightness = std::clamp(value, -1.0f, 1.0f);
            WriteFloatConfig(kBrightnessConfigKey, passthroughBrightness);
            applySliderChange();
          });

          BSML::Lite::CreateSliderSetting(settingsParent, "Contrast", 0.01f, passthroughContrast, -1.0f, 1.0f, 0.05f, true, {0.0f, 0.0f}, [applySliderChange](float value) {
            passthroughContrast = std::clamp(value, -1.0f, 1.0f);
            WriteFloatConfig(kContrastConfigKey, passthroughContrast);
            applySliderChange();
          });

          BSML::Lite::CreateSliderSetting(settingsParent, "Saturation", 0.01f, passthroughSaturation, -1.0f, 1.0f, 0.05f, true, {0.0f, 0.0f}, [applySliderChange](float value) {
            passthroughSaturation = std::clamp(value, -1.0f, 1.0f);
            WriteFloatConfig(kSaturationConfigKey, passthroughSaturation);
            applySliderChange();
          });
        },
        BSML::MenuType::All);
  } catch (...) {
    AndroidInfo("BSML gameplay setup toggle registration threw an exception.");
    PaperLogger.warn("BSML gameplay setup toggle registration threw an exception.");
    return;
  }

  if (bsmlRegistered) {
    PaperLogger.info("Registered gameplay setup toggle.");
  } else {
    PaperLogger.warn("Failed to register gameplay setup toggle.");
  }
}
} // namespace

MAKE_HOOK_MATCH(OVROverlay_CreateLayer, &::GlobalNamespace::OVROverlay::CreateLayer, bool, ::GlobalNamespace::OVROverlay* self, int32_t mipLevels, int32_t sampleCount,
                ::GlobalNamespace::OVRPlugin_EyeTextureFormat etFormat, int32_t flags, ::GlobalNamespace::OVRPlugin_Sizei size, ::GlobalNamespace::OVRPlugin_OverlayShape shape) {
  int32_t beforeLayerId = self ? self->get_layerId() : -1;
  bool result = OVROverlay_CreateLayer(self, mipLevels, sampleCount, etFormat, flags, size, shape);

  if (IsTransparencyOverlay(self) && overlayCreateHookLogsRemaining > 0) {
    overlayCreateHookLogsRemaining--;
    int32_t afterLayerId = self ? self->get_layerId() : -1;
    __android_log_print(ANDROID_LOG_INFO, "Transparency",
                        "OVROverlay CreateLayer result=%d beforeId=%d afterId=%d mip=%d samples=%d format=%d flags=%d size=%dx%d shape=%d", result, beforeLayerId, afterLayerId,
                        mipLevels, sampleCount, static_cast<int32_t>(etFormat), flags, size.w, size.h, static_cast<int32_t>(shape));
    PaperLogger.info("OVROverlay CreateLayer result={}, beforeId={}, afterId={}, mip={}, samples={}, format={}, flags={}, size={}x{}, shape={}", result, beforeLayerId, afterLayerId,
                     mipLevels, sampleCount, static_cast<int32_t>(etFormat), flags, size.w, size.h, static_cast<int32_t>(shape));
  }

  return result;
}

MAKE_HOOK_MATCH(OVROverlay_SubmitLayer, &::GlobalNamespace::OVROverlay::SubmitLayer, bool, ::GlobalNamespace::OVROverlay* self, bool overlay, bool headLocked,
                bool noDepthBufferTesting, ::GlobalNamespace::OVRPose pose, ::UnityEngine::Vector3 scale, int32_t frameIndex) {
  bool result = OVROverlay_SubmitLayer(self, overlay, headLocked, noDepthBufferTesting, pose, scale, frameIndex);

  if (IsTransparencyOverlay(self) && overlaySubmitHookLogsRemaining > 0) {
    overlaySubmitHookLogsRemaining--;
    int32_t layerId = self ? self->get_layerId() : -1;
    __android_log_print(ANDROID_LOG_INFO, "Transparency", "OVROverlay SubmitLayer result=%d layerId=%d overlay=%d headLocked=%d noDepth=%d frame=%d scale=%.3f,%.3f,%.3f",
                        result, layerId, overlay, headLocked, noDepthBufferTesting, frameIndex, scale.x, scale.y, scale.z);
    PaperLogger.info("OVROverlay SubmitLayer result={}, layerId={}, overlay={}, headLocked={}, noDepth={}, frame={}, scale={},{},{}", result, layerId, overlay, headLocked,
                     noDepthBufferTesting, frameIndex, scale.x, scale.y, scale.z);
  }

  return result;
}

MAKE_HOOK_MATCH(GameplayCoreInstaller_InstallBindings, &::GlobalNamespace::GameplayCoreInstaller::InstallBindings, void, ::GlobalNamespace::GameplayCoreInstaller* self) {
  GameplayCoreInstaller_InstallBindings(self);
  EnterLevel();
}

MAKE_HOOK_MATCH(EnvironmentSceneSetup_InstallBindings, &::GlobalNamespace::EnvironmentSceneSetup::InstallBindings, void, ::GlobalNamespace::EnvironmentSceneSetup* self) {
  EnvironmentSceneSetup_InstallBindings(self);
  if (inLevel) {
    sceneRefreshPasses = 12;
    ApplyEffectIfNeeded();
  }
}

MAKE_HOOK_MATCH(StandardLevelGameplayManager_Update, &::GlobalNamespace::StandardLevelGameplayManager::Update, void, ::GlobalNamespace::StandardLevelGameplayManager* self) {
  StandardLevelGameplayManager_Update(self);

  if (sceneRefreshPasses > 0) {
    sceneRefreshPasses--;
    if ((sceneRefreshPasses % 3) == 0) {
      ApplyEffectIfNeeded();
    }
  }

  SubmitOverlayLayerDirectly("StandardLevelGameplayManager_Update");
}

MAKE_HOOK_MATCH(StandardLevelGameplayManager_HandleSongDidFinish, &::GlobalNamespace::StandardLevelGameplayManager::HandleSongDidFinish, void,
                ::GlobalNamespace::StandardLevelGameplayManager* self) {
  ExitLevel();
  StandardLevelGameplayManager_HandleSongDidFinish(self);
}

MAKE_HOOK_MATCH(StandardLevelGameplayManager_OnDestroy, &::GlobalNamespace::StandardLevelGameplayManager::OnDestroy, void, ::GlobalNamespace::StandardLevelGameplayManager* self) {
  ExitLevel();
  StandardLevelGameplayManager_OnDestroy(self);
}

// Loads the config from disk using our modInfo, then returns it for use
// other config tools such as config-utils don't use this config, so it can be
// removed if those are in use
Configuration &getConfig() {
  static Configuration config(modInfo);
  return config;
}

// Called at the early stages of game loading
MOD_EXTERN_FUNC void setup(CModInfo *info) noexcept {
  AndroidInfo("setup entered");
  *info = modInfo.to_c();

  getConfig().Load();
  passthroughEnabled = ReadEnabledConfig();
  passthroughOpacity = ReadFloatConfig(kOpacityConfigKey, kDefaultPassthroughOpacity, 0.0f, 1.0f);
  passthroughBrightness = ReadFloatConfig(kBrightnessConfigKey, kDefaultPassthroughBrightness, -1.0f, 1.0f);
  passthroughContrast = ReadFloatConfig(kContrastConfigKey, kDefaultPassthroughContrast, -1.0f, 1.0f);
  passthroughSaturation = ReadFloatConfig(kSaturationConfigKey, kDefaultPassthroughSaturation, -1.0f, 1.0f);

  // File logging
  Paper::Logger::RegisterFileContextId(PaperLogger.tag);

  PaperLogger.info("Completed setup!");
}

// Called later on in the game loading - a good time to install function hooks
MOD_EXTERN_FUNC void late_load() noexcept {
  AndroidInfo("late_load entered");
  il2cpp_functions::Init();

  PaperLogger.info("Installing hooks...");

  INSTALL_HOOK(PaperLogger, GameplayCoreInstaller_InstallBindings);
  INSTALL_HOOK(PaperLogger, EnvironmentSceneSetup_InstallBindings);
  INSTALL_HOOK(PaperLogger, StandardLevelGameplayManager_Update);
  INSTALL_HOOK(PaperLogger, StandardLevelGameplayManager_HandleSongDidFinish);
  INSTALL_HOOK(PaperLogger, StandardLevelGameplayManager_OnDestroy);
  INSTALL_HOOK(PaperLogger, OVROverlay_CreateLayer);
  INSTALL_HOOK(PaperLogger, OVROverlay_SubmitLayer);
  AndroidInfo("hooks installed");

  RegisterGameplaySetupToggle();

  PaperLogger.info("Installed all hooks!");
  AndroidInfo("late_load completed");
}
