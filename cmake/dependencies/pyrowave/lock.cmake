# Public inputs plus the reviewed, unpublished local codec commit. Never fetch PIN.
set(APOLLO_PYROWAVE_PIN "5e4a98f807dddd2498824e3b55ef2fe1845bcc59")
set(PYROWAVE_BASE "89f7e47d4abbf650c91fae766728af866c5e32a0")
set(PYROWAVE_TREE "ad55a253c952a59bf180f9e52ead021b6de38f26")
set(PYROWAVE_URL "https://github.com/Themaister/pyrowave.git")
set(PYROWAVE_GRANITE_PIN "1b2d1801d2910fb09ebcded2f0bb3a3a781103b5")
set(PYROWAVE_GRANITE_URL "https://github.com/Themaister/Granite.git")
# Minimal shipping backend: no renderer, runtime shader compiler or reflection.
# The reviewed generated shaders are tracked in the codec, not regenerated.
set(PYROWAVE_SUBMODULE_PATHS third_party/volk third_party/khronos/vulkan-headers)
set(PYROWAVE_SUBMODULE_PINS
    "47cddf7ed97b94118a08aacb548a411188e016cc"
    "6802bb4733b63ed5efd3adb308a6c885ef180ea1")
set(PYROWAVE_SUBMODULE_URLS
    "https://github.com/zeux/volk"
    "https://github.com/KhronosGroup/Vulkan-Headers")
