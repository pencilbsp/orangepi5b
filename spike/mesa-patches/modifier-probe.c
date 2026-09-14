/* Ask panvk for DRM format modifiers both ways and print what each returns.
 *
 * config/patches/mesa-26.0.8/0002 exists because panvk fills
 * VkDrmFormatModifierPropertiesListEXT but leaves
 * VkDrmFormatModifierPropertiesList2EXT at zero, and
 * gnome-remote-desktop 50 only asks the 2 way. This prints both counts side by
 * side for the formats GRD looks at, so "v1=1 v2=0" before the patch and
 * "v1=1 v2=1" after is visible rather than inferred.
 *
 * Cross-built by build.sh; run on the board against either Mesa variant.
 */
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

static const struct {
   VkFormat format;
   const char *name;
   const char *drm;
} formats[] = {
   { VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM", "XR24 / AR24" },
   { VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", "XB24 / AB24" },
   { VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM_PACK32", "AB30" },
   { VK_FORMAT_A2R10G10B10_UNORM_PACK32, "A2R10G10B10_UNORM_PACK32", "AR30" },
   { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, "G8_B8R8_2PLANE_420_UNORM", "NV12" },
};

int
main(void)
{
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "modifier-probe",
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance instance;
   VkResult r = vkCreateInstance(&ci, NULL, &instance);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "vkCreateInstance failed: %d\n", r);
      return 1;
   }

   uint32_t n_devices = 0;
   vkEnumeratePhysicalDevices(instance, &n_devices, NULL);
   if (n_devices == 0) {
      fprintf(stderr, "no Vulkan physical device\n");
      return 1;
   }
   VkPhysicalDevice devices[8];
   if (n_devices > 8)
      n_devices = 8;
   vkEnumeratePhysicalDevices(instance, &n_devices, devices);

   for (uint32_t d = 0; d < n_devices; d++) {
      VkPhysicalDeviceProperties props;
      vkGetPhysicalDeviceProperties(devices[d], &props);
      printf("device: %s\n", props.deviceName);

      for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
         VkDrmFormatModifierPropertiesListEXT list = {
            .sType =
               VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
         };
         VkFormatProperties2 fp = {
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
            .pNext = &list,
         };
         vkGetPhysicalDeviceFormatProperties2(devices[d], formats[i].format,
                                              &fp);

         VkDrmFormatModifierPropertiesList2EXT list2 = {
            .sType =
               VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT,
         };
         VkFormatProperties2 fp2 = {
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
            .pNext = &list2,
         };
         vkGetPhysicalDeviceFormatProperties2(devices[d], formats[i].format,
                                              &fp2);

         /* GRD logs the modifier it settles on, so print the values too:
          * matching lists mean the v1 fallback and the v2 answer are
          * interchangeable, not merely both non-empty. */
         VkDrmFormatModifierPropertiesEXT p1[8];
         VkDrmFormatModifierProperties2EXT p2[8];
         char m1[128] = "", m2[128] = "";

         if (list.drmFormatModifierCount &&
             list.drmFormatModifierCount <= 8) {
            list.pDrmFormatModifierProperties = p1;
            vkGetPhysicalDeviceFormatProperties2(devices[d],
                                                 formats[i].format, &fp);
            for (uint32_t k = 0; k < list.drmFormatModifierCount; k++)
               snprintf(m1 + strlen(m1), sizeof(m1) - strlen(m1), "%s0x%llx",
                        k ? "," : "",
                        (unsigned long long)p1[k].drmFormatModifier);
         }
         if (list2.drmFormatModifierCount &&
             list2.drmFormatModifierCount <= 8) {
            list2.pDrmFormatModifierProperties = p2;
            vkGetPhysicalDeviceFormatProperties2(devices[d],
                                                 formats[i].format, &fp2);
            for (uint32_t k = 0; k < list2.drmFormatModifierCount; k++)
               snprintf(m2 + strlen(m2), sizeof(m2) - strlen(m2), "%s0x%llx",
                        k ? "," : "",
                        (unsigned long long)p2[k].drmFormatModifier);
         }

         printf("  %-26s %-12s v1=%u [%s]  v2=%u [%s]\n", formats[i].name,
                formats[i].drm, list.drmFormatModifierCount, m1,
                list2.drmFormatModifierCount, m2);
      }
   }

   vkDestroyInstance(instance, NULL);
   return 0;
}
