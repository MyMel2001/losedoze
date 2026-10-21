/* losvulkan.c — guest-side Vulkan probe.
 *
 * Creates a Vulkan instance, picks a physical device (lavapipe in the
 * image), creates a logical device, submits an empty command buffer and
 * waits for it. Prints a machine-checkable marker:
 *
 *   LOSEDOZE VULKAN OK <device name>
 */
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  VkApplicationInfo app = {0};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "losvulkan";
  app.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo ici = {0};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;

  VkInstance inst = VK_NULL_HANDLE;
  VkResult r = vkCreateInstance(&ici, NULL, &inst);
  if (r != VK_SUCCESS) {
    printf("LOSEDOZE VULKAN FAIL vkCreateInstance %d\n", (int)r);
    return 1;
  }

  uint32_t count = 0;
  r = vkEnumeratePhysicalDevices(inst, &count, NULL);
  if (r != VK_SUCCESS || count == 0) {
    printf("LOSEDOZE VULKAN FAIL no physical device (%d)\n", (int)r);
    return 1;
  }
  VkPhysicalDevice *devs = calloc(count, sizeof *devs);
  if (!devs) {
    printf("LOSEDOZE VULKAN FAIL oom\n");
    return 1;
  }
  vkEnumeratePhysicalDevices(inst, &count, devs);

  VkPhysicalDevice chosen = devs[0];
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(chosen, &props);

  uint32_t qcount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(chosen, &qcount, NULL);
  VkQueueFamilyProperties *qfam = calloc(qcount ? qcount : 1, sizeof *qfam);
  vkGetPhysicalDeviceQueueFamilyProperties(chosen, &qcount, qfam);
  uint32_t family = 0;
  for (uint32_t i = 0; i < qcount; i++) {
    if (qfam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
  }
  free(qfam);

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci = {0};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;

  VkDeviceCreateInfo dci = {0};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;

  VkDevice dev = VK_NULL_HANDLE;
  r = vkCreateDevice(chosen, &dci, NULL, &dev);
  if (r != VK_SUCCESS) {
    printf("LOSEDOZE VULKAN FAIL vkCreateDevice %d\n", (int)r);
    return 1;
  }

  VkCommandPoolCreateInfo cpci = {0};
  cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpci.queueFamilyIndex = family;
  VkCommandPool pool = VK_NULL_HANDLE;
  vkCreateCommandPool(dev, &cpci, NULL, &pool);

  VkCommandBufferAllocateInfo cbai = {0};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(dev, &cbai, &cmd);

  VkCommandBufferBeginInfo cbbi = {0};
  cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &cbbi);
  vkEndCommandBuffer(cmd);

  VkFenceCreateInfo fci = {0};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(dev, &fci, NULL, &fence);

  VkSubmitInfo si = {0};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;

  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(dev, family, 0, &queue);
  r = vkQueueSubmit(queue, 1, &si, fence);
  if (r != VK_SUCCESS) {
    printf("LOSEDOZE VULKAN FAIL vkQueueSubmit %d\n", (int)r);
    return 1;
  }
  r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5ull * 1000 * 1000 * 1000);
  if (r != VK_SUCCESS) {
    printf("LOSEDOZE VULKAN FAIL vkWaitForFences %d\n", (int)r);
    return 1;
  }

  printf("LOSEDOZE VULKAN OK %s | API %u.%u.%u | driver %u\n",
         props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
         VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion),
         props.driverVersion);
  fflush(stdout);

  vkDestroyFence(dev, fence, NULL);
  vkFreeCommandBuffers(dev, pool, 1, &cmd);
  vkDestroyCommandPool(dev, pool, NULL);
  vkDestroyDevice(dev, NULL);
  vkDestroyInstance(inst, NULL);
  free(devs);
  return 0;
}
