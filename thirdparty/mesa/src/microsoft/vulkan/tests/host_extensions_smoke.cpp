#define VK_NO_PROTOTYPES
#include <cstdio>
#include <cstring>
#include <vector>
#include <vulkan/vulkan.h>
#include <windows.h>

int main(int argc, char **argv) {
   if (argc != 3)
      return 2;
   if (!LoadLibraryA(argv[2]))
      return 2;
   LoadLibraryW(L"d3d12.dll");
   LoadLibraryW(L"dxgi.dll");
   HMODULE module = LoadLibraryA(argv[1]);
   if (!module) {
      printf("LOAD FAIL %lu\n", GetLastError());
      return 1;
   }
   auto gip = (PFN_vkGetInstanceProcAddr)GetProcAddress(module, "vk_icdGetInstanceProcAddr");
   if (!gip)
      return 1;
   auto createInstance = (PFN_vkCreateInstance)gip(VK_NULL_HANDLE, "vkCreateInstance");
   VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
   app.apiVersion = VK_API_VERSION_1_2;
   VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
   ici.pApplicationInfo = &app;
   VkInstance instance;
   VkResult result = createInstance(&ici, nullptr, &instance);
   printf("instance %d\n", result);
   if (result)
      return 1;
#define INST(name) auto name = (PFN_##name)gip(instance, #name)
   INST(vkEnumeratePhysicalDevices);
   INST(vkEnumerateDeviceExtensionProperties);
   INST(vkGetPhysicalDeviceFeatures2);
   INST(vkGetPhysicalDeviceProperties2);
   INST(vkGetPhysicalDeviceMemoryProperties2);
   INST(vkGetPhysicalDeviceExternalBufferProperties);
   INST(vkGetPhysicalDeviceQueueFamilyProperties);
   INST(vkCreateDevice);
   INST(vkDestroyInstance);
   INST(vkGetDeviceProcAddr);
   uint32_t count = 0;
   vkEnumeratePhysicalDevices(instance, &count, nullptr);
   std::vector<VkPhysicalDevice> devices(count);
   vkEnumeratePhysicalDevices(instance, &count, devices.data());
   int failures = 0;
   for (auto physical : devices) {
      uint32_t ec = 0;
      vkEnumerateDeviceExtensionProperties(physical, nullptr, &ec, nullptr);
      std::vector<VkExtensionProperties> extensions(ec);
      vkEnumerateDeviceExtensionProperties(physical, nullptr, &ec, extensions.data());
      bool border = false, host = false;
      for (auto &e : extensions) {
         border |= !strcmp(e.extensionName, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
         host |= !strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
      }
      printf("custom_border_color=%d external_memory_host=%d\n", border, host);
      VkPhysicalDeviceCustomBorderColorFeaturesEXT bf = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
      VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &bf};
      vkGetPhysicalDeviceFeatures2(physical, &features);
      VkPhysicalDeviceCustomBorderColorPropertiesEXT border_props = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT};
      VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT, &border_props};
      VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                &host_props};
      vkGetPhysicalDeviceProperties2(physical, &properties);
      printf("geometryPointSize=%u largePoints=%u pointSizeRange=[%g,%g]\n",
             features.features.shaderTessellationAndGeometryPointSize,
             features.features.largePoints,
             properties.properties.limits.pointSizeRange[0],
             properties.properties.limits.pointSizeRange[1]);
      if (!features.features.shaderTessellationAndGeometryPointSize ||
          features.features.largePoints ||
          properties.properties.limits.pointSizeRange[0] != 1.0f ||
          properties.properties.limits.pointSizeRange[1] != 1.0f)
         ++failures;
      VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
      VkPhysicalDeviceMemoryProperties2 memory = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget};
      for (unsigned query = 0; query < 1024; ++query) {
         memset(budget.heapBudget, 0xff, sizeof(budget.heapBudget));
         memset(budget.heapUsage, 0xff, sizeof(budget.heapUsage));
         vkGetPhysicalDeviceMemoryProperties2(physical, &memory);
         for (unsigned heap = 0; heap < VK_MAX_MEMORY_HEAPS; ++heap) {
            if (heap >= memory.memoryProperties.memoryHeapCount) {
               if (budget.heapBudget[heap] || budget.heapUsage[heap])
                  ++failures;
            } else if (budget.heapBudget[heap] > memory.memoryProperties.memoryHeaps[heap].size) {
               ++failures;
            }
         }
      }
      for (unsigned heap = 0; heap < memory.memoryProperties.memoryHeapCount; ++heap)
         printf("heap=%u budget=%llu usage=%llu\n", heap,
                (unsigned long long)budget.heapBudget[heap],
                (unsigned long long)budget.heapUsage[heap]);
      printf("adapter=%s maxCustomBorderColorSamplers=%u hostAlignment=%llu\n",
             properties.properties.deviceName, border_props.maxCustomBorderColorSamplers,
             (unsigned long long)host_props.minImportedHostPointerAlignment);
      if (border && (!bf.customBorderColors || !bf.customBorderColorWithoutFormat ||
                     !border_props.maxCustomBorderColorSamplers))
         ++failures;
      if (!border && (bf.customBorderColors || bf.customBorderColorWithoutFormat))
         ++failures;
      VkPhysicalDeviceExternalBufferInfo external_info = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
      external_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
      VkExternalBufferProperties external_props = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
      vkGetPhysicalDeviceExternalBufferProperties(physical, &external_info, &external_props);
      if (host && (!(external_props.externalMemoryProperties.externalMemoryFeatures &
                     VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
                   external_props.externalMemoryProperties.exportFromImportedHandleTypes ||
                   external_props.externalMemoryProperties.compatibleHandleTypes !=
                       external_info.handleType))
         ++failures;
      if (!border)
         printf("SKIP custom sampler tests: unsupported runtime\n");
      if (!host)
         printf("SKIP host import tests: unsupported runtime\n");
      uint32_t qc = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &qc, nullptr);
      std::vector<VkQueueFamilyProperties> queues(qc);
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &qc, queues.data());
      uint32_t qi = 0;
      while (qi < qc && !(queues[qi].queueFlags & VK_QUEUE_GRAPHICS_BIT))
         ++qi;
      if (qi == qc) {
         ++failures;
         continue;
      }
      float priority = 1;
      VkDeviceQueueCreateInfo queue = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      queue.queueFamilyIndex = qi;
      queue.queueCount = 1;
      queue.pQueuePriorities = &priority;
      const char *enabled[3];
      uint32_t ne = 0;
      if (border)
         enabled[ne++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
      if (host)
         enabled[ne++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
      enabled[ne++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
      VkPhysicalDeviceFeatures requested = {};
      requested.geometryShader = features.features.geometryShader;
      requested.shaderTessellationAndGeometryPointSize = VK_TRUE;
      VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      dci.pEnabledFeatures = &requested;
      dci.pNext = border ? &bf : nullptr;
      dci.queueCreateInfoCount = 1;
      dci.pQueueCreateInfos = &queue;
      dci.enabledExtensionCount = ne;
      dci.ppEnabledExtensionNames = enabled;
      VkDevice device;
      result = vkCreateDevice(physical, &dci, nullptr, &device);
      printf("device %d\n", result);
      if (result) {
         ++failures;
         continue;
      }
#define DEV(name) auto name = (PFN_##name)vkGetDeviceProcAddr(device, #name)
      DEV(vkDestroyDevice);
      DEV(vkCreateSampler);
      DEV(vkDestroySampler);
      DEV(vkGetMemoryHostPointerPropertiesEXT);
      DEV(vkAllocateMemory);
      DEV(vkFreeMemory);
      DEV(vkMapMemory);
      DEV(vkUnmapMemory);
      DEV(vkCreateBuffer);
      DEV(vkDestroyBuffer);
      DEV(vkGetBufferMemoryRequirements);
      DEV(vkBindBufferMemory);
      if (border) {
         for (unsigned integer = 0; integer < 4; ++integer) {
            VkSamplerCustomBorderColorCreateInfoEXT color = {
                VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
            color.format = integer >= 2    ? VK_FORMAT_UNDEFINED
                           : (integer & 1) ? VK_FORMAT_R32G32B32A32_UINT
                                           : VK_FORMAT_R32G32B32A32_SFLOAT;
            color.customBorderColor.float32[0] = 0.25f;
            color.customBorderColor.uint32[1] = (integer & 1) ? 42 : 0;
            VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, &color};
            sci.addressModeU = sci.addressModeV = sci.addressModeW =
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            sci.borderColor =
                (integer & 1) ? VK_BORDER_COLOR_INT_CUSTOM_EXT : VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            VkSampler sampler;
            result = vkCreateSampler(device, &sci, nullptr, &sampler);
            printf("sampler integer=%u result=%d\n", integer, result);
            if (result)
               ++failures;
            else
               vkDestroySampler(device, sampler, nullptr);
         }
      }
      if (host) {
         void *pointer = VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
         VkMemoryHostPointerPropertiesEXT props = {
             VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
         result = vkGetMemoryHostPointerPropertiesEXT(
             device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, pointer, &props);
         printf("host properties=%d bits=%x\n", result, props.memoryTypeBits);
         if (result)
            ++failures;
         else {
            VkExternalMemoryBufferCreateInfo external = {
                VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
            external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
            bci.size = 65536;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VkBuffer buffer;
            result = vkCreateBuffer(device, &bci, nullptr, &buffer);
            if (result)
               ++failures;
            else {
               VkMemoryRequirements req;
               vkGetBufferMemoryRequirements(device, buffer, &req);
               uint32_t bits = req.memoryTypeBits & props.memoryTypeBits;
               if (!bits)
                  ++failures;
               else {
                  uint32_t type = 0;
                  while (!(bits & (1u << type)))
                     ++type;
                  VkImportMemoryHostPointerInfoEXT import = {
                      VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
                  import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
                  import.pHostPointer = pointer;
                  VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import, 65536,
                                             type};
                  VkDeviceMemory memory;
                  result = vkAllocateMemory(device, &ai, nullptr, &memory);
                  printf("import=%d\n", result);
                  if (result)
                     ++failures;
                  else {
                     result = vkBindBufferMemory(device, buffer, memory, 0);
                     printf("bind=%d\n", result);
                     if (result)
                        ++failures;
                     void *mapped = nullptr;
                     result = vkMapMemory(device, memory, 0, 65536, 0, &mapped);
                     if (result || mapped != pointer)
                        ++failures;
                     else {
                        memset(mapped, 0x5a, 65536);
                        vkUnmapMemory(device, memory);
                     }
                     vkDestroyBuffer(device, buffer, nullptr);
                     buffer = VK_NULL_HANDLE;
                     vkFreeMemory(device, memory, nullptr);
                     if (*(unsigned char *)pointer != 0x5a)
                        ++failures;
                  }
               }
               if (buffer)
                  vkDestroyBuffer(device, buffer, nullptr);
            }
         }
         result = vkGetMemoryHostPointerPropertiesEXT(
             device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (char *)pointer + 1,
             &props);
         if (result != VK_ERROR_INVALID_EXTERNAL_HANDLE || props.memoryTypeBits)
            ++failures;
         result = vkGetMemoryHostPointerPropertiesEXT(
             device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT, pointer,
             &props);
         if (result != VK_ERROR_INVALID_EXTERNAL_HANDLE || props.memoryTypeBits)
            ++failures;
         VirtualFree(pointer, 0, MEM_RELEASE);
      }
      vkDestroyDevice(device, nullptr);
   }
   vkDestroyInstance(instance, nullptr);
   FreeLibrary(module);
   printf("devices=%u failures=%d\n", count, failures);
   return count && !failures ? 0 : 1;
}
