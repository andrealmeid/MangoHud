/*
 * Copyright © 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <list>

#include <sys/types.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "imgui.h"

#include "overlay.h"
#include "font_default.h"

// #include "util/debug.h"
#include <inttypes.h>
#include "mesa/util/macros.h"
#include "mesa/util/os_time.h"
#include "mesa/util/os_socket.h"

#include "vk_enum_to_str.h"
#include <vulkan/vk_util.h>

#include "string_utils.h"
#include "file_utils.h"
#include "gpu.h"
#include "logging.h"
#include "keybinds.h"
#include "cpu.h"
#include "loaders/loader_nvml.h"
#include "memory.h"
#include "notify.h"
#include "blacklist.h"
#include "version.h"

#ifdef HAVE_DBUS
#include "dbus_info.h"
float g_overflow = 50.f /* 3333ms * 0.5 / 16.6667 / 2 (to edge and back) */;
#endif

bool open = false;
string gpuString;
float offset_x, offset_y, hudSpacing;
int hudFirstRow, hudSecondRow;
struct amdGpu amdgpu;
struct fps_limit fps_limit_stats;
string log_name;
std::thread write_thread;
ofstream log_file;

/* Mapped from VkInstace/VkPhysicalDevice */
struct instance_data {
   struct vk_instance_dispatch_table vtable;
   VkInstance instance;

   struct overlay_params params;

   bool first_line_printed;

   int control_client;

   /* Dumping of frame stats to a file has been enabled. */
   bool capture_enabled;

   /* Dumping of frame stats to a file has been enabled and started. */
   bool capture_started;

   string engineName, engineVersion;
   notify_thread notifier;
};

/* Mapped from VkDevice */
struct queue_data;
struct device_data {
   struct instance_data *instance;

   PFN_vkSetDeviceLoaderData set_device_loader_data;

   struct vk_device_dispatch_table vtable;
   VkPhysicalDevice physical_device;
   VkDevice device;

   VkPhysicalDeviceProperties properties;

   struct queue_data *graphic_queue;

   std::vector<struct queue_data *> queues;
};

/* Mapped from VkCommandBuffer */
struct queue_data;
struct command_buffer_data {
   struct device_data *device;

   VkCommandBufferLevel level;

   VkCommandBuffer cmd_buffer;

   struct queue_data *queue_data;
};

/* Mapped from VkQueue */
struct queue_data {
   struct device_data *device;

   VkQueue queue;
   VkQueueFlags flags;
   uint32_t family_index;
};

struct overlay_draw {
   VkCommandBuffer command_buffer;

   VkSemaphore cross_engine_semaphore;

   VkSemaphore semaphore;
   VkFence fence;

   VkBuffer vertex_buffer;
   VkDeviceMemory vertex_buffer_mem;
   VkDeviceSize vertex_buffer_size;

   VkBuffer index_buffer;
   VkDeviceMemory index_buffer_mem;
   VkDeviceSize index_buffer_size;
};

/* Mapped from VkSwapchainKHR */
struct swapchain_data {
   struct device_data *device;

   VkSwapchainKHR swapchain;
   unsigned width, height;
   VkFormat format;

   std::vector<VkImage> images;
   std::vector<VkImageView> image_views;
   std::vector<VkFramebuffer> framebuffers;

   VkRenderPass render_pass;

   VkDescriptorPool descriptor_pool;
   VkDescriptorSetLayout descriptor_layout;
   VkDescriptorSet descriptor_set;

   VkSampler font_sampler;

   VkPipelineLayout pipeline_layout;
   VkPipeline pipeline;

   VkCommandPool command_pool;

   std::list<overlay_draw *> draws; /* List of struct overlay_draw */

   ImFont* font = nullptr;
   bool font_uploaded;
   VkImage font_image;
   VkImageView font_image_view;
   VkDeviceMemory font_mem;
   VkBuffer upload_font_buffer;
   VkDeviceMemory upload_font_buffer_mem;

   /**/
   ImGuiContext* imgui_context;
   ImVec2 window_size;

   /**/
   uint64_t last_present_time;

   unsigned n_frames_since_update;
   uint64_t last_fps_update;
   double frametime;
   double frametimeDisplay;
   const char* cpuString;
   const char* gpuString;

   struct swapchain_stats sw_stats;

   /* Over a single frame */
   struct frame_stat frame_stats;

   /* Over fps_sampling_period */
   struct frame_stat accumulated_stats;
};

// single global lock, for simplicity
std::mutex global_lock;
typedef std::lock_guard<std::mutex> scoped_lock;
std::unordered_map<uint64_t, void *> vk_object_to_data;

thread_local ImGuiContext* __MesaImGui;

#define HKEY(obj) ((uint64_t)(obj))
#define FIND(type, obj) (reinterpret_cast<type *>(find_object_data(HKEY(obj))))

static void *find_object_data(uint64_t obj)
{
   scoped_lock lk(global_lock);
   return vk_object_to_data[obj];
}

static void map_object(uint64_t obj, void *data)
{
   scoped_lock lk(global_lock);
   vk_object_to_data[obj] = data;
}

static void unmap_object(uint64_t obj)
{
   scoped_lock lk(global_lock);
   vk_object_to_data.erase(obj);
}

/**/

#define VK_CHECK(expr) \
   do { \
      VkResult __result = (expr); \
      if (__result != VK_SUCCESS) { \
         fprintf(stderr, "'%s' line %i failed with %s\n", \
                 #expr, __LINE__, vk_Result_to_str(__result)); \
      } \
   } while (0)

/**/

static VkLayerInstanceCreateInfo *get_instance_chain_info(const VkInstanceCreateInfo *pCreateInfo,
                                                          VkLayerFunction func)
{
   vk_foreach_struct(item, pCreateInfo->pNext) {
      if (item->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
          ((VkLayerInstanceCreateInfo *) item)->function == func)
         return (VkLayerInstanceCreateInfo *) item;
   }
   unreachable("instance chain info not found");
   return NULL;
}

static VkLayerDeviceCreateInfo *get_device_chain_info(const VkDeviceCreateInfo *pCreateInfo,
                                                      VkLayerFunction func)
{
   vk_foreach_struct(item, pCreateInfo->pNext) {
      if (item->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
          ((VkLayerDeviceCreateInfo *) item)->function == func)
         return (VkLayerDeviceCreateInfo *)item;
   }
   unreachable("device chain info not found");
   return NULL;
}

/**/

static struct instance_data *new_instance_data(VkInstance instance)
{
   struct instance_data *data = new instance_data();
   data->instance = instance;
   data->control_client = -1;
   map_object(HKEY(data->instance), data);
   return data;
}

static void destroy_instance_data(struct instance_data *data)
{
   if (data->params.control >= 0)
      os_socket_close(data->params.control);
   unmap_object(HKEY(data->instance));
   delete data;
}

static void instance_data_map_physical_devices(struct instance_data *instance_data,
                                               bool map)
{
   uint32_t physicalDeviceCount = 0;
   instance_data->vtable.EnumeratePhysicalDevices(instance_data->instance,
                                                  &physicalDeviceCount,
                                                  NULL);

   std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
   instance_data->vtable.EnumeratePhysicalDevices(instance_data->instance,
                                                  &physicalDeviceCount,
                                                  physicalDevices.data());

   for (uint32_t i = 0; i < physicalDeviceCount; i++) {
      if (map)
         map_object(HKEY(physicalDevices[i]), instance_data);
      else
         unmap_object(HKEY(physicalDevices[i]));
   }
}

/**/
static struct device_data *new_device_data(VkDevice device, struct instance_data *instance)
{
   struct device_data *data = new device_data();
   data->instance = instance;
   data->device = device;
   map_object(HKEY(data->device), data);
   return data;
}

static struct queue_data *new_queue_data(VkQueue queue,
                                         const VkQueueFamilyProperties *family_props,
                                         uint32_t family_index,
                                         struct device_data *device_data)
{
   struct queue_data *data = new queue_data();
   data->device = device_data;
   data->queue = queue;
   data->flags = family_props->queueFlags;
   data->family_index = family_index;
   map_object(HKEY(data->queue), data);

   if (data->flags & VK_QUEUE_GRAPHICS_BIT)
      device_data->graphic_queue = data;

   return data;
}

static void destroy_queue(struct queue_data *data)
{
   unmap_object(HKEY(data->queue));
   delete data;
}

static void device_map_queues(struct device_data *data,
                              const VkDeviceCreateInfo *pCreateInfo)
{
   uint32_t n_queues = 0;
   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++)
      n_queues += pCreateInfo->pQueueCreateInfos[i].queueCount;
   data->queues.resize(n_queues);

   struct instance_data *instance_data = data->instance;
   uint32_t n_family_props;
   instance_data->vtable.GetPhysicalDeviceQueueFamilyProperties(data->physical_device,
                                                                &n_family_props,
                                                                NULL);
   std::vector<VkQueueFamilyProperties> family_props(n_family_props);
   instance_data->vtable.GetPhysicalDeviceQueueFamilyProperties(data->physical_device,
                                                                &n_family_props,
                                                                family_props.data());

   uint32_t queue_index = 0;
   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      for (uint32_t j = 0; j < pCreateInfo->pQueueCreateInfos[i].queueCount; j++) {
         VkQueue queue;
         data->vtable.GetDeviceQueue(data->device,
                                     pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex,
                                     j, &queue);

         VK_CHECK(data->set_device_loader_data(data->device, queue));

         data->queues[queue_index++] =
            new_queue_data(queue, &family_props[pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex],
                           pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex, data);
      }
   }
}

static void device_unmap_queues(struct device_data *data)
{
   for (auto q : data->queues)
      destroy_queue(q);
}

static void destroy_device_data(struct device_data *data)
{
   unmap_object(HKEY(data->device));
   delete data;
}

/**/
static struct command_buffer_data *new_command_buffer_data(VkCommandBuffer cmd_buffer,
                                                           VkCommandBufferLevel level,
                                                           struct device_data *device_data)
{
   struct command_buffer_data *data = new command_buffer_data();
   data->device = device_data;
   data->cmd_buffer = cmd_buffer;
   data->level = level;
   map_object(HKEY(data->cmd_buffer), data);
   return data;
}

static void destroy_command_buffer_data(struct command_buffer_data *data)
{
   unmap_object(HKEY(data->cmd_buffer));
   delete data;
}

/**/
static struct swapchain_data *new_swapchain_data(VkSwapchainKHR swapchain,
                                                 struct device_data *device_data)
{
   struct instance_data *instance_data = device_data->instance;
   struct swapchain_data *data = new swapchain_data();
   data->device = device_data;
   data->swapchain = swapchain;
   data->window_size = ImVec2(instance_data->params.width, instance_data->params.height);
   map_object(HKEY(data->swapchain), data);
   return data;
}

static void destroy_swapchain_data(struct swapchain_data *data)
{
   unmap_object(HKEY(data->swapchain));
   delete data;
}

struct overlay_draw *get_overlay_draw(struct swapchain_data *data)
{
   struct device_data *device_data = data->device;
   struct overlay_draw *draw = data->draws.empty() ?
      nullptr : data->draws.front();

   VkSemaphoreCreateInfo sem_info = {};
   sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

   if (draw && device_data->vtable.GetFenceStatus(device_data->device, draw->fence) == VK_SUCCESS) {
      VK_CHECK(device_data->vtable.ResetFences(device_data->device,
                                               1, &draw->fence));
      data->draws.pop_front();
      data->draws.push_back(draw);
      return draw;
   }

   draw = new overlay_draw();

   VkCommandBufferAllocateInfo cmd_buffer_info = {};
   cmd_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   cmd_buffer_info.commandPool = data->command_pool;
   cmd_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cmd_buffer_info.commandBufferCount = 1;
   VK_CHECK(device_data->vtable.AllocateCommandBuffers(device_data->device,
                                                       &cmd_buffer_info,
                                                       &draw->command_buffer));
   VK_CHECK(device_data->set_device_loader_data(device_data->device,
                                                draw->command_buffer));


   VkFenceCreateInfo fence_info = {};
   fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
   VK_CHECK(device_data->vtable.CreateFence(device_data->device,
                                            &fence_info,
                                            NULL,
                                            &draw->fence));

   VK_CHECK(device_data->vtable.CreateSemaphore(device_data->device, &sem_info,
                                                NULL, &draw->semaphore));
   VK_CHECK(device_data->vtable.CreateSemaphore(device_data->device, &sem_info,
                                                NULL, &draw->cross_engine_semaphore));

   data->draws.push_back(draw);

   return draw;
}

static void parse_command(struct instance_data *instance_data,
                          const char *cmd, unsigned cmdlen,
                          const char *param, unsigned paramlen)
{
   if (!strncmp(cmd, "capture", cmdlen)) {
      int value = atoi(param);
      bool enabled = value > 0;

      if (enabled) {
         instance_data->capture_enabled = true;
      } else {
         instance_data->capture_enabled = false;
         instance_data->capture_started = false;
      }
   }
}

#define BUFSIZE 4096

/**
 * This function will process commands through the control file.
 *
 * A command starts with a colon, followed by the command, and followed by an
 * option '=' and a parameter.  It has to end with a semi-colon. A full command
 * + parameter looks like:
 *
 *    :cmd=param;
 */
static void process_char(struct instance_data *instance_data, char c)
{
   static char cmd[BUFSIZE];
   static char param[BUFSIZE];

   static unsigned cmdpos = 0;
   static unsigned parampos = 0;
   static bool reading_cmd = false;
   static bool reading_param = false;

   switch (c) {
   case ':':
      cmdpos = 0;
      parampos = 0;
      reading_cmd = true;
      reading_param = false;
      break;
   case ';':
      if (!reading_cmd)
         break;
      cmd[cmdpos++] = '\0';
      param[parampos++] = '\0';
      parse_command(instance_data, cmd, cmdpos, param, parampos);
      reading_cmd = false;
      reading_param = false;
      break;
   case '=':
      if (!reading_cmd)
         break;
      reading_param = true;
      break;
   default:
      if (!reading_cmd)
         break;

      if (reading_param) {
         /* overflow means an invalid parameter */
         if (parampos >= BUFSIZE - 1) {
            reading_cmd = false;
            reading_param = false;
            break;
         }

         param[parampos++] = c;
      } else {
         /* overflow means an invalid command */
         if (cmdpos >= BUFSIZE - 1) {
            reading_cmd = false;
            break;
         }

         cmd[cmdpos++] = c;
      }
   }
}

static void control_send(struct instance_data *instance_data,
                         const char *cmd, unsigned cmdlen,
                         const char *param, unsigned paramlen)
{
   unsigned msglen = 0;
   char buffer[BUFSIZE];

   assert(cmdlen + paramlen + 3 < BUFSIZE);

   buffer[msglen++] = ':';

   memcpy(&buffer[msglen], cmd, cmdlen);
   msglen += cmdlen;

   if (paramlen > 0) {
      buffer[msglen++] = '=';
      memcpy(&buffer[msglen], param, paramlen);
      msglen += paramlen;
      buffer[msglen++] = ';';
   }

   os_socket_send(instance_data->control_client, buffer, msglen, 0);
}

static void control_send_connection_string(struct device_data *device_data)
{
   struct instance_data *instance_data = device_data->instance;

   const char *controlVersionCmd = "MesaOverlayControlVersion";
   const char *controlVersionString = "1";

   control_send(instance_data, controlVersionCmd, strlen(controlVersionCmd),
                controlVersionString, strlen(controlVersionString));

   const char *deviceCmd = "DeviceName";
   const char *deviceName = device_data->properties.deviceName;

   control_send(instance_data, deviceCmd, strlen(deviceCmd),
                deviceName, strlen(deviceName));

   const char *mesaVersionCmd = "MesaVersion";
   const char *mesaVersionString = "Mesa " PACKAGE_VERSION;

   control_send(instance_data, mesaVersionCmd, strlen(mesaVersionCmd),
                mesaVersionString, strlen(mesaVersionString));
}

static void control_client_check(struct device_data *device_data)
{
   struct instance_data *instance_data = device_data->instance;

   /* Already connected, just return. */
   if (instance_data->control_client >= 0)
      return;

   int socket = os_socket_accept(instance_data->params.control);
   if (socket == -1) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNABORTED)
         fprintf(stderr, "ERROR on socket: %s\n", strerror(errno));
      return;
   }

   if (socket >= 0) {
      os_socket_block(socket, false);
      instance_data->control_client = socket;
      control_send_connection_string(device_data);
   }
}

static void control_client_disconnected(struct instance_data *instance_data)
{
   os_socket_close(instance_data->control_client);
   instance_data->control_client = -1;
}

static void process_control_socket(struct instance_data *instance_data)
{
   const int client = instance_data->control_client;
   if (client >= 0) {
      char buf[BUFSIZE];

      while (true) {
         ssize_t n = os_socket_recv(client, buf, BUFSIZE, 0);

         if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
               /* nothing to read, try again later */
               break;
            }

            if (errno != ECONNRESET)
               fprintf(stderr, "ERROR on connection: %s\n", strerror(errno));

            control_client_disconnected(instance_data);
         } else if (n == 0) {
            /* recv() returns 0 when the client disconnects */
            control_client_disconnected(instance_data);
         }

         for (ssize_t i = 0; i < n; i++) {
            process_char(instance_data, buf[i]);
         }

         /* If we try to read BUFSIZE and receive BUFSIZE bytes from the
          * socket, there's a good chance that there's still more data to be
          * read, so we will try again. Otherwise, simply be done for this
          * iteration and try again on the next frame.
          */
         if (n < BUFSIZE)
            break;
      }
   }
}

string exec(string command) {
   char buffer[128];
   string result = "";

   // Open pipe to file
   FILE* pipe = popen(command.c_str(), "r");
   if (!pipe) {
      return "popen failed!";
   }

   // read till end of process:
   while (!feof(pipe)) {

      // use buffer to read and add to result
      if (fgets(buffer, 128, pipe) != NULL)
         result += buffer;
   }

   pclose(pipe);
   return result;
}

void init_cpu_stats(overlay_params& params)
{
   auto& enabled = params.enabled;
   enabled[OVERLAY_PARAM_ENABLED_cpu_stats] = cpuStats.Init()
                           && enabled[OVERLAY_PARAM_ENABLED_cpu_stats];
   enabled[OVERLAY_PARAM_ENABLED_cpu_temp] = cpuStats.GetCpuFile()
                           && enabled[OVERLAY_PARAM_ENABLED_cpu_temp];
}

struct PCI_BUS {
   int domain;
   int bus;
   int slot;
   int func;
};

void init_gpu_stats(uint32_t& vendorID, overlay_params& params)
{
   if (!params.enabled[OVERLAY_PARAM_ENABLED_gpu_stats])
      return;

   PCI_BUS pci;
   bool pci_bus_parsed = false;
   const char *pci_dev = nullptr;
   if (!params.pci_dev.empty())
      pci_dev = params.pci_dev.c_str();

   // for now just checks if pci bus parses correctly, if at all necessary
   if (pci_dev) {
      if (sscanf(pci_dev, "%04x:%02x:%02x.%x",
               &pci.domain, &pci.bus,
               &pci.slot, &pci.func) == 4) {
         pci_bus_parsed = true;
         // reformat back to sysfs file name's and nvml's expected format
         // so config file param's value format doesn't have to be as strict
         std::stringstream ss;
         ss << std::hex
            << std::setw(4) << std::setfill('0') << pci.domain << ":"
            << std::setw(2) << pci.bus << ":"
            << std::setw(2) << pci.slot << "."
            << std::setw(1) << pci.func;
         params.pci_dev = ss.str();
         pci_dev = params.pci_dev.c_str();
#ifndef NDEBUG
         std::cerr << "MANGOHUD: PCI device ID: '" << pci_dev << "'\n";
#endif
      } else {
         std::cerr << "MANGOHUD: Failed to parse PCI device ID: '" << pci_dev << "'\n";
         std::cerr << "MANGOHUD: Specify it as 'domain:bus:slot.func'\n";
      }
   }

   // NVIDIA or Intel but maybe has Optimus
   if (vendorID == 0x8086
      || vendorID == 0x10de) {

      bool nvSuccess = (checkNVML(pci_dev) && getNVMLInfo());

#ifdef HAVE_XNVCTRL
      if (!nvSuccess)
         nvSuccess = checkXNVCtrl();
#endif

      if ((params.enabled[OVERLAY_PARAM_ENABLED_gpu_stats] = nvSuccess)) {
         vendorID = 0x10de;
      }
   }

   if (vendorID == 0x8086 || vendorID == 0x1002
       || gpu.find("Radeon") != std::string::npos
       || gpu.find("AMD") != std::string::npos) {
      string path;
      string drm = "/sys/class/drm/";

      auto dirs = ls(drm.c_str(), "card");
      for (auto& dir : dirs) {
         path = drm + dir;

#ifndef NDEBUG
         std::cerr << "amdgpu path check: " << path << "/device/vendor" << std::endl;
#endif

         string line = read_line(path + "/device/vendor");
         trim(line);
         if (line != "0x1002" || !file_exists(path + "/device/gpu_busy_percent"))
            continue;

         path += "/device";
         if (pci_bus_parsed && pci_dev) {
            string pci_device = readlink(path.c_str());
#ifndef NDEBUG
            std::cerr << "PCI device symlink: " << pci_device << "\n";
#endif
            if (!ends_with(pci_device, pci_dev)) {
               std::cerr << "MANGOHUD: skipping GPU, no PCI ID match\n";
               continue;
            }
         }

#ifndef NDEBUG
           std::cerr << "using amdgpu path: " << path << std::endl;
#endif

         if (!amdGpuFile)
            amdGpuFile = fopen((path + "/gpu_busy_percent").c_str(), "r");
         if (!amdGpuVramTotalFile)
            amdGpuVramTotalFile = fopen((path + "/mem_info_vram_total").c_str(), "r");
         if (!amdGpuVramUsedFile)
            amdGpuVramUsedFile = fopen((path + "/mem_info_vram_used").c_str(), "r");

         path += "/hwmon/";
         string tempFolder;
         if (find_folder(path, "hwmon", tempFolder)) {
            if (!amdGpuCoreClockFile)
               amdGpuCoreClockFile = fopen((path + tempFolder + "/freq1_input").c_str(), "r");
            if (!amdGpuMemoryClockFile)
               amdGpuMemoryClockFile = fopen((path + tempFolder + "/freq2_input").c_str(), "r");
            if (!amdTempFile)
               amdTempFile = fopen((path + tempFolder + "/temp1_input").c_str(), "r");

            params.enabled[OVERLAY_PARAM_ENABLED_gpu_stats] = true;
            vendorID = 0x1002;
            break;
         }
      }

      // don't bother then
      if (!amdGpuFile && !amdTempFile && !amdGpuVramTotalFile && !amdGpuVramUsedFile) {
         params.enabled[OVERLAY_PARAM_ENABLED_gpu_stats] = false;
      }
   }
}

void init_system_info(){
      const char* ld_preload = getenv("LD_PRELOAD");
      if (ld_preload)
         unsetenv("LD_PRELOAD");

      ram =  exec("cat /proc/meminfo | grep 'MemTotal' | awk '{print $2}'");
      trim(ram);
      cpu =  exec("cat /proc/cpuinfo | grep 'model name' | tail -n1 | sed 's/^.*: //' | sed 's/([^)]*)/()/g' | tr -d '(/)'");
      trim(cpu);
      kernel = exec("uname -r");
      trim(kernel);
      os = exec("cat /etc/*-release | grep 'PRETTY_NAME' | cut -d '=' -f 2-");
      os.erase(remove(os.begin(), os.end(), '\"' ), os.end());
      trim(os);
      gpu = exec("lspci | grep VGA | head -n1 | awk -vRS=']' -vFS='[' '{print $2}' | sed '/^$/d' | tail -n1");
      trim(gpu);
      driver = exec("glxinfo | grep 'OpenGL version' | sed 's/^.*: //' | cut -d' ' --output-delimiter=$'\n' -f1- | grep -v '(' | grep -v ')' | tr '\n' ' ' | cut -c 1-");
      trim(driver);
      //driver = itox(device_data->properties.driverVersion);

      if (ld_preload)
         setenv("LD_PRELOAD", ld_preload, 1);
#ifndef NDEBUG
      std::cout << "Ram:" << ram << "\n"
                << "Cpu:" << cpu << "\n"
                << "Kernel:" << kernel << "\n"
                << "Os:" << os << "\n"
                << "Gpu:" << gpu << "\n"
                << "Driver:" << driver << std::endl;
#endif

      if (!log_period_env || !try_stoi(log_period, log_period_env)){
        log_period = 100;
      } else {
         out.open("/tmp/mango", ios::out | ios::app);
      }
}

void check_keybinds(struct overlay_params& params){
   bool pressed = false; // FIXME just a placeholder until wayland support
   uint64_t now = os_time_get(); /* us */
   elapsedF2 = (double)(now - last_f2_press);
   elapsedF12 = (double)(now - last_f12_press);
   elapsedReloadCfg = (double)(now - reload_cfg_press);
  
  if (elapsedF2 >= 500000 && !params.output_file.empty()){
#ifdef HAVE_X11
     pressed = key_is_pressed(params.toggle_logging);
#else
     pressed = false;
#endif
     if (pressed){
       last_f2_press = now;
       log_start = now;
       loggingOn = !loggingOn;

       if (loggingOn)
         log_file = create_file(params.output_file);
       else {
         swap_array(&write_buffer, &read_buffer);
         std::thread t = std::thread(write_file, &log_file);
         t.join();
	 log_file.flush();
         log_file.close();
         log_size = log_i;
         log_i = 0;
       }
     }
   }

   if (elapsedF12 >= 500000){
#ifdef HAVE_X11
      pressed = key_is_pressed(params.toggle_hud);
#else
      pressed = false;
#endif
      if (pressed){
         last_f12_press = now;
         params.no_display = !params.no_display;
      }
   }

   if (elapsedReloadCfg >= 500000){
#ifdef HAVE_X11
      pressed = key_is_pressed(params.reload_cfg);
#else
      pressed = false;
#endif
      if (pressed){
         parse_overlay_config(&params, getenv("MANGOHUD_CONFIG"));
         reload_cfg_press = now;
      }
   }
}

void update_hud_info(struct swapchain_stats& sw_stats, struct overlay_params& params, uint32_t vendorID){
   uint32_t f_idx = sw_stats.n_frames % ARRAY_SIZE(sw_stats.frames_stats);
   uint64_t now = os_time_get(); /* us */

   double elapsed = (double)(now - sw_stats.last_fps_update); /* us */
   long int frame_time = now - sw_stats.last_present_time;
   fps = 1000000.0f * sw_stats.n_frames_since_update / elapsed;
   if (sw_stats.last_present_time) {
        sw_stats.frames_stats[f_idx].stats[OVERLAY_PLOTS_frame_timing] =
	  frame_time;
   }

   if (sw_stats.last_fps_update) {
      if (elapsed >= params.fps_sampling_period) {

         if (params.enabled[OVERLAY_PARAM_ENABLED_cpu_stats]) {
            cpuStats.UpdateCPUData();
            sw_stats.total_cpu = cpuStats.GetCPUDataTotal().percent;

            if (params.enabled[OVERLAY_PARAM_ENABLED_core_load])
               cpuStats.UpdateCoreMhz();
            if (params.enabled[OVERLAY_PARAM_ENABLED_cpu_temp])
               cpuStats.UpdateCpuTemp();
         }

         if (params.enabled[OVERLAY_PARAM_ENABLED_gpu_stats]) {
            if (vendorID == 0x1002)
               std::thread(getAmdGpuUsage).detach();

            if (vendorID == 0x10de)
               std::thread(getNvidiaGpuInfo).detach();
         }

         // get ram usage/max
         if (params.enabled[OVERLAY_PARAM_ENABLED_ram])
            std::thread(update_meminfo).detach();
         if (params.enabled[OVERLAY_PARAM_ENABLED_io_read] || params.enabled[OVERLAY_PARAM_ENABLED_io_write])
            std::thread(getIoStats, &sw_stats.io).detach();
            
         gpuLoadLog = gpu_info.load;
         cpuLoadLog = sw_stats.total_cpu;
         sw_stats.fps = fps;

         if (params.enabled[OVERLAY_PARAM_ENABLED_time]) {
            std::time_t t = std::time(nullptr);
            std::stringstream time;
            time << std::put_time(std::localtime(&t), params.time_format.c_str());
            sw_stats.time = time.str();
         }

         sw_stats.n_frames_since_update = 0;
         sw_stats.last_fps_update = now;

      }
   } else {
      sw_stats.last_fps_update = now;
   }

   sw_stats.last_present_time = now;
   sw_stats.n_frames++;
   sw_stats.n_frames_since_update++;

   if (loggingOn) {
     elapsedLog = now - log_start;
     if (params.log_duration && (elapsedLog) >= params.log_duration * 1000000) {
       loggingOn = false;
       swap_array(&write_buffer, &read_buffer);
       std::thread t = std::thread(write_file, &log_file);
       t.join();
       log_file.flush();
       log_file.close();
       log_size = log_i;
       log_i = 0;
     } else {
       //logArray.push_back({fps, frame_time, cpuLoadLog, gpuLoadLog, elapsedLog});
       write_buffer[log_i].fps = fps;
       write_buffer[log_i].frametime = frame_time;
       write_buffer[log_i].cpu = cpuLoadLog;
       write_buffer[log_i].gpu = gpuLoadLog;
       write_buffer[log_i].previous = elapsedLog;
       log_i++;
     }

     if (log_i == BUF_SIZE) {
	/*
        if (write_thread.joinable())
	    write_thread.join();
	    */

	swap_array(&write_buffer, &read_buffer);
	log_size = log_i;
	log_i = 0;
	std::thread(write_file, &log_file).detach();
     }
   }

}

static void snapshot_swapchain_frame(struct swapchain_data *data)
{
   struct device_data *device_data = data->device;
   struct instance_data *instance_data = device_data->instance;
   update_hud_info(data->sw_stats, instance_data->params, device_data->properties.vendorID);
   check_keybinds(instance_data->params);

   // not currently used
   // if (instance_data->params.control >= 0) {
   //    control_client_check(device_data);
   //    process_control_socket(instance_data);
   // }
}

static float get_time_stat(void *_data, int _idx)
{
   struct swapchain_stats *data = (struct swapchain_stats *) _data;
   if ((ARRAY_SIZE(data->frames_stats) - _idx) > data->n_frames)
      return 0.0f;
   int idx = ARRAY_SIZE(data->frames_stats) +
      data->n_frames < ARRAY_SIZE(data->frames_stats) ?
      _idx - data->n_frames :
      _idx + data->n_frames;
   idx %= ARRAY_SIZE(data->frames_stats);
   /* Time stats are in us. */
   return data->frames_stats[idx].stats[data->stat_selector] / data->time_dividor;
}

void position_layer(struct overlay_params& params, ImVec2 window_size)
{
   unsigned width = ImGui::GetIO().DisplaySize.x;
   unsigned height = ImGui::GetIO().DisplaySize.y;
   float margin = 10.0f;
   if (params.offset_x > 0 || params.offset_y > 0)
      margin = 0.0f;

   ImGui::SetNextWindowBgAlpha(params.background_alpha);
   ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);

   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
   ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8,-3));
   ImGui::PushStyleVar(ImGuiStyleVar_Alpha, params.alpha);

   switch (params.position) {
   case LAYER_POSITION_TOP_LEFT:
      ImGui::SetNextWindowPos(ImVec2(margin + params.offset_x, margin + params.offset_y), ImGuiCond_Always);
      break;
   case LAYER_POSITION_TOP_RIGHT:
      ImGui::SetNextWindowPos(ImVec2(width - window_size.x - margin + params.offset_x, margin + params.offset_y),
                              ImGuiCond_Always);
      break;
   case LAYER_POSITION_BOTTOM_LEFT:
      ImGui::SetNextWindowPos(ImVec2(margin + params.offset_x, height - window_size.y - margin + params.offset_y),
                              ImGuiCond_Always);
      break;
   case LAYER_POSITION_BOTTOM_RIGHT:
      ImGui::SetNextWindowPos(ImVec2(width - window_size.x - margin + params.offset_x,
                                     height - window_size.y - margin + params.offset_y),
                              ImGuiCond_Always);
      break;
   }
}

static void right_aligned_text(float off_x, const char *fmt, ...)
{
   ImVec2 pos = ImGui::GetCursorPos();
   char buffer[32] {};

   va_list args;
   va_start(args, fmt);
   vsnprintf(buffer, sizeof(buffer), fmt, args);
   va_end(args);

   ImVec2 sz = ImGui::CalcTextSize(buffer);
   ImGui::SetCursorPosX(pos.x + off_x - sz.x);
   ImGui::Text("%s", buffer);
}

float get_ticker_limited_pos(float pos, float tw, float& left_limit, float& right_limit)
{
   float cw = ImGui::GetContentRegionAvailWidth();
   float new_pos_x = ImGui::GetCursorPosX();
   left_limit = cw - tw + new_pos_x;
   right_limit = new_pos_x;

   if (cw < tw) {
      new_pos_x += pos;
      // acts as a delay before it starts scrolling again
      if (new_pos_x < left_limit)
         return left_limit;
      else if (new_pos_x > right_limit)
         return right_limit;
      else
         return new_pos_x;
   }
   return new_pos_x;
}

#ifdef HAVE_DBUS
void render_mpris_metadata(swapchain_stats& data, metadata& meta, uint64_t frame_timing)
{
   scoped_lock lk(meta.mutex);
   if (meta.valid) {
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8,0));
      ImGui::Dummy(ImVec2(0.0f, 20.0f));
      ImGui::PushFont(data.font1);

      if (meta.ticker.needs_recalc) {
         meta.ticker.tw0 = ImGui::CalcTextSize(meta.title.c_str()).x;
         meta.ticker.tw1 = ImGui::CalcTextSize(meta.artists.c_str()).x;
         meta.ticker.tw2 = ImGui::CalcTextSize(meta.album.c_str()).x;
         meta.ticker.longest = std::max(std::max(
               meta.ticker.tw0,
               meta.ticker.tw1),
            meta.ticker.tw2);
         meta.ticker.needs_recalc = false;
      }

      float new_pos, left_limit = 0, right_limit = 0;
      get_ticker_limited_pos(meta.ticker.pos, meta.ticker.longest, left_limit, right_limit);

      if (meta.ticker.pos < left_limit - g_overflow * .5f) {
         meta.ticker.dir = -1;
         meta.ticker.pos = (left_limit - g_overflow * .5f) + 1.f /* random */;
      } else if (meta.ticker.pos > right_limit + g_overflow) {
         meta.ticker.dir = 1;
         meta.ticker.pos = (right_limit + g_overflow) - 1.f /* random */;
      }

      meta.ticker.pos -= .5f * (frame_timing / 16666.7f) * meta.ticker.dir;

      new_pos = get_ticker_limited_pos(meta.ticker.pos, meta.ticker.tw0, left_limit, right_limit);
      ImGui::SetCursorPosX(new_pos);
      ImGui::Text("%s", meta.title.c_str());

      new_pos = get_ticker_limited_pos(meta.ticker.pos, meta.ticker.tw1, left_limit, right_limit);
      ImGui::SetCursorPosX(new_pos);
      ImGui::Text("%s", meta.artists.c_str());
      //ImGui::NewLine();
      if (!meta.album.empty()) {
         new_pos = get_ticker_limited_pos(meta.ticker.pos, meta.ticker.tw2, left_limit, right_limit);
         ImGui::SetCursorPosX(new_pos);
         ImGui::Text("%s", meta.album.c_str());
      }
      ImGui::PopFont();
      ImGui::PopStyleVar();
   }
}
#endif

void render_imgui(swapchain_stats& data, struct overlay_params& params, ImVec2& window_size, bool is_vulkan)
{
   uint32_t f_idx = (data.n_frames - 1) % ARRAY_SIZE(data.frames_stats);
   uint64_t frame_timing = data.frames_stats[f_idx].stats[OVERLAY_PLOTS_frame_timing];
   static float char_width = ImGui::CalcTextSize("A").x;
   window_size = ImVec2(params.width, params.height);
   unsigned width = ImGui::GetIO().DisplaySize.x;
   unsigned height = ImGui::GetIO().DisplaySize.y;
      
   if (!params.no_display){
      ImGui::Begin("Main", &open, ImGuiWindowFlags_NoDecoration);
      if (params.enabled[OVERLAY_PARAM_ENABLED_version]){
         ImGui::Text("%s", MANGOHUD_VERSION);
         ImGui::Dummy(ImVec2(0, 8.0f));
      }
      if (params.enabled[OVERLAY_PARAM_ENABLED_time]){
         ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.00f), "%s", data.time.c_str());
      }
      ImGui::BeginTable("hud", params.tableCols);
      if (params.enabled[OVERLAY_PARAM_ENABLED_gpu_stats]){
         ImGui::TableNextRow();
         ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(params.gpu_color), "GPU");
         ImGui::TableNextCell();
         right_aligned_text(char_width * 4, "%i", gpu_info.load);
         ImGui::SameLine(0, 1.0f);
         ImGui::Text("%%");
         // ImGui::SameLine(150);
         // ImGui::Text("%s", "%");
         if (params.enabled[OVERLAY_PARAM_ENABLED_gpu_temp]){
            ImGui::TableNextCell();
            right_aligned_text(char_width * 4, "%i", gpu_info.temp);
            ImGui::SameLine(0, 1.0f);
            ImGui::Text("°C");
         }
         if (params.enabled[OVERLAY_PARAM_ENABLED_gpu_core_clock]){
            ImGui::TableNextCell();
            right_aligned_text(char_width * 4, "%i", gpu_info.CoreClock);
            ImGui::SameLine(0, 1.0f);
            ImGui::PushFont(data.font1);
            ImGui::Text("MHz");
            ImGui::PopFont();
         }
      }
      if(params.enabled[OVERLAY_PARAM_ENABLED_cpu_stats]){
         ImGui::TableNextRow();
         ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(params.cpu_color), "CPU");
         ImGui::TableNextCell();
         right_aligned_text(char_width * 4, "%d", data.total_cpu);
         ImGui::SameLine(0, 1.0f);
         ImGui::Text("%%");
         // ImGui::SameLine(150);
         // ImGui::Text("%s", "%");
      
         if (params.enabled[OVERLAY_PARAM_ENABLED_cpu_temp]){
            ImGui::TableNextCell();
            right_aligned_text(char_width * 4, "%i", cpuStats.GetCPUDataTotal().temp);
            ImGui::SameLine(0, 1.0f);
            ImGui::Text("°C");
         }
      }
      
      if (params.enabled[OVERLAY_PARAM_ENABLED_core_load]){
         int i = 0;
         for (const CPUData &cpuData : cpuStats.GetCPUData())
         {
            ImGui::TableNextRow();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(params.cpu_color), "CPU");
            ImGui::SameLine(0, 1.0f);
            ImGui::PushFont(data.font1);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(params.cpu_color),"%i", i);
            ImGui::PopFont();
            ImGui::TableNextCell();
            right_aligned_text(char_width * 4, "%i", int(cpuData.percent));
            ImGui::SameLine(0, 1.0f);
            ImGui::Text("%%");
            ImGui::TableNextCell();
            right_aligned_text(char_width * 4, "%i", cpuData.mhz);
            ImGui::SameLine(0, 1.0f);
            ImGui::PushFont(data.font1);
            ImGui::Text("MHz");
            ImGui::PopFont();
            i++;
         }
      }
      if (params.enabled[OVERLAY_PARAM_ENABLED_io_read] || params.enabled[OVERLAY_PARAM_ENABLED_io_write]){
         auto sampling = params.fps_sampling_period;
         ImGui::TableNextRow();
         if (params.enabled[OVERLAY_PARAM_ENABLED_io_read] && !params.enabled[OVERLAY_PARAM_ENABLED_io_write])
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(params.io_color), "IO RD");
         if (params.enabled[OVERLAY_PARAM_ENABLED_io_write] && !params.enabled[OVERLAY_PARAM_ENABLED_io_read])
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(params.io_color), "IO RW");
         if (params.enabled[OVERLAY_PARAM_ENABLED_io_read] && params.enabled[OVERLAY_PARAM_ENABLED_io_write])
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(params.io_color), "IO RD/RW");
         
         if (params.enabled[OVERLAY_PARAM_ENABLED_io_read]){
            ImGui::TableNextCell();
            float val = data.io.diff.read * 1000000 / sampling;
            right_aligned_text(char_width * 4, val < 100 ? "%.2f" : "%.f", val);
            ImGui::SameLine(0,1.0f);
            ImGui::PushFont(data.font1);
            ImGui::Text("MiB/s");
            ImGui::PopFont();
         }
         if (params.enabled[OVERLAY_PARAM_ENABLED_io_write]){
            ImGui::TableNextCell();
            float val = data.io.diff.write * 1000000 / sampling;
            right_aligned_text(char_width * 4, val < 100 ? "%.2f" : "%.f", val);
            ImGui::SameLine(0,1.0f);
            ImGui::PushFont(data.font1);
            ImGui::Text("MiB/s");
            ImGui::PopFont();
         }
      }
      if (params.enabled[OVERLAY_PARAM_ENABLED_vram]){
         ImGui::TableNextRow();
         ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(params.vram_color), "VRAM");
         ImGui::TableNextCell();
         right_aligned_text(char_width * 4, "%.2f", gpu_info.memoryUsed);
         ImGui::SameLine(0,1.0f);
         ImGui::PushFont(data.font1);
         ImGui::Text("GiB");
         ImGui::PopFont();
         if (params.enabled[OVERLAY_PARAM_ENABLED_gpu_mem_clock]){
            ImGui::TableNextCell();
            right_aligned_text(char_width * 4, "%i", gpu_info.MemClock);
            ImGui::SameLine(0, 1.0f);
            ImGui::PushFont(data.font1);
            ImGui::Text("MHz");
            ImGui::PopFont();
         }
      }
      if (params.enabled[OVERLAY_PARAM_ENABLED_ram]){
         ImGui::TableNextRow();
         ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(params.ram_color), "RAM");
         ImGui::TableNextCell();
         right_aligned_text(char_width * 4, "%.2f", memused);
         ImGui::SameLine(0,1.0f);
         ImGui::PushFont(data.font1);
         ImGui::Text("GiB");
         ImGui::PopFont();
      }
      if (params.enabled[OVERLAY_PARAM_ENABLED_fps]){
         ImGui::TableNextRow();
         ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(params.engine_color), "%s", is_vulkan ? data.engineName.c_str() : "OpenGL");
         ImGui::TableNextCell();
         right_aligned_text(char_width * 4, "%.0f", data.fps);
         ImGui::SameLine(0, 1.0f);
         ImGui::PushFont(data.font1);
         ImGui::Text("FPS");
         ImGui::PopFont();
         ImGui::TableNextCell();
         right_aligned_text(char_width * 4, "%.1f", 1000 / data.fps);
         ImGui::SameLine(0, 1.0f);
         ImGui::PushFont(data.font1);
         ImGui::Text("ms");
         ImGui::PopFont();
      }
      ImGui::EndTable();

      if (params.enabled[OVERLAY_PARAM_ENABLED_fps]){
         ImGui::PushFont(data.font1);
         ImGui::Dummy(ImVec2(0, 8.0f));
         auto engine_color = ImGui::ColorConvertU32ToFloat4(params.engine_color);
         if (is_vulkan) {
            if ((data.engineName == "DXVK" || data.engineName == "VKD3D")){
               ImGui::TextColored(engine_color,
                  "%s/%d.%d.%d", data.engineVersion.c_str(),
                  data.version_vk.major,
                  data.version_vk.minor,
                  data.version_vk.patch);
            } else {
               ImGui::TextColored(engine_color,
                  "%d.%d.%d",
                  data.version_vk.major,
                  data.version_vk.minor,
                  data.version_vk.patch);
            }
         } else {
            ImGui::TextColored(engine_color,
               "%d.%d%s", data.version_gl.major, data.version_gl.minor,
               data.version_gl.is_gles ? " ES" : "");
         }
         ImGui::SameLine();
         ImGui::TextColored(engine_color,
                 "/ %s", data.deviceName.c_str());
         if (params.enabled[OVERLAY_PARAM_ENABLED_arch]){
            ImGui::Dummy(ImVec2(0.0,5.0f));
            ImGui::TextColored(engine_color, "%s", "" MANGOHUD_ARCH);
         }
         ImGui::PopFont();
      }

      /*
      if (loggingOn && log_period == 0){
         uint64_t now = os_time_get();
         elapsedLog = (double)(now - log_start);
         if ((elapsedLog) >= params.log_duration * 1000000)
            // loggingOn = false;

         out << fps << "," <<  cpuLoadLog << "," << gpuLoadLog << "," << (now - log_start) << endl;
      }
      */

      if (params.enabled[OVERLAY_PARAM_ENABLED_frame_timing]){
         ImGui::Dummy(ImVec2(0.0f, params.font_size / 2));
         ImGui::PushFont(data.font1);
         ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(params.engine_color), "%s", "Frametime");
         ImGui::PopFont();

         char hash[40];
         snprintf(hash, sizeof(hash), "##%s", overlay_param_names[OVERLAY_PARAM_ENABLED_frame_timing]);
         data.stat_selector = OVERLAY_PLOTS_frame_timing;
         data.time_dividor = 1000.0f;

         ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
         double min_time = 0.0f;
         double max_time = 50.0f;
         ImGui::PlotLines(hash, get_time_stat, &data,
                              ARRAY_SIZE(data.frames_stats), 0,
                              NULL, min_time, max_time,
                              ImVec2(ImGui::GetContentRegionAvailWidth() - params.font_size * 2.2, 50));
         ImGui::PopStyleColor();
      }
      if (params.enabled[OVERLAY_PARAM_ENABLED_frame_timing]){
         ImGui::SameLine(0,1.0f);
         ImGui::PushFont(data.font1);
         ImGui::Text("%.1f ms", 1000 / data.fps); //frame_timing / 1000.f);
         ImGui::PopFont();
      }

#ifdef HAVE_DBUS
      render_mpris_metadata(data, spotify, frame_timing);
      render_mpris_metadata(data, generic_mpris, frame_timing);
#endif

      window_size = ImVec2(window_size.x, ImGui::GetCursorPosY() + 10.0f);
      ImGui::End();
      if(loggingOn){
         ImGui::SetNextWindowBgAlpha(0.0);
         ImGui::SetNextWindowSize(ImVec2(params.font_size * 13, params.font_size * 13), ImGuiCond_Always);
         ImGui::SetNextWindowPos(ImVec2(width - params.font_size * 13,
                                       0),
                                       ImGuiCond_Always);
         ImGui::Begin("Logging", &open, ImGuiWindowFlags_NoDecoration);
         ImGui::Text("Logging...");
         ImGui::Text("Elapsed: %isec", int((elapsedLog) / 1000000));
         ImGui::End();
      }
      if (params.enabled[OVERLAY_PARAM_ENABLED_crosshair]){
         ImGui::SetNextWindowBgAlpha(0.0);
         ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
         ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
         ImGui::Begin("Logging", &open, ImGuiWindowFlags_NoDecoration);
         ImVec2 horiz = ImVec2(width / 2 - (params.crosshair_size / 2), height / 2);
         ImVec2 vert = ImVec2(width / 2, height / 2 - (params.crosshair_size / 2));
         ImGui::GetWindowDrawList()->AddLine(horiz,
            ImVec2(horiz.x + params.crosshair_size, horiz.y + 0),
            params.crosshair_color, 2.0f);
         ImGui::GetWindowDrawList()->AddLine(vert,
            ImVec2(vert.x + 0, vert.y + params.crosshair_size),
            params.crosshair_color, 2.0f);
         ImGui::End();
      }
   }
}

static void compute_swapchain_display(struct swapchain_data *data)
{
   struct device_data *device_data = data->device;
   struct instance_data *instance_data = device_data->instance;

   ImGui::SetCurrentContext(data->imgui_context);
   ImGui::NewFrame();
   {
      scoped_lock lk(instance_data->notifier.mutex);
      position_layer(instance_data->params, data->window_size);
      render_imgui(data->sw_stats, instance_data->params, data->window_size, true);
   }
   ImGui::PopStyleVar(3);

   ImGui::EndFrame();
   ImGui::Render();
}

static uint32_t vk_memory_type(struct device_data *data,
                               VkMemoryPropertyFlags properties,
                               uint32_t type_bits)
{
    VkPhysicalDeviceMemoryProperties prop;
    data->instance->vtable.GetPhysicalDeviceMemoryProperties(data->physical_device, &prop);
    for (uint32_t i = 0; i < prop.memoryTypeCount; i++)
        if ((prop.memoryTypes[i].propertyFlags & properties) == properties && type_bits & (1<<i))
            return i;
    return 0xFFFFFFFF; // Unable to find memoryType
}

static void ensure_swapchain_fonts(struct swapchain_data *data,
                                   VkCommandBuffer command_buffer)
{
   if (data->font_uploaded)
      return;

   data->font_uploaded = true;

   struct device_data *device_data = data->device;
   ImGuiIO& io = ImGui::GetIO();
   unsigned char* pixels;
   int width, height;
   io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
   size_t upload_size = width * height * 4 * sizeof(char);

   /* Upload buffer */
   VkBufferCreateInfo buffer_info = {};
   buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   buffer_info.size = upload_size;
   buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
   buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   VK_CHECK(device_data->vtable.CreateBuffer(device_data->device, &buffer_info,
                                             NULL, &data->upload_font_buffer));
   VkMemoryRequirements upload_buffer_req;
   device_data->vtable.GetBufferMemoryRequirements(device_data->device,
                                                   data->upload_font_buffer,
                                                   &upload_buffer_req);
   VkMemoryAllocateInfo upload_alloc_info = {};
   upload_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   upload_alloc_info.allocationSize = upload_buffer_req.size;
   upload_alloc_info.memoryTypeIndex = vk_memory_type(device_data,
                                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                                      upload_buffer_req.memoryTypeBits);
   VK_CHECK(device_data->vtable.AllocateMemory(device_data->device,
                                               &upload_alloc_info,
                                               NULL,
                                               &data->upload_font_buffer_mem));
   VK_CHECK(device_data->vtable.BindBufferMemory(device_data->device,
                                                 data->upload_font_buffer,
                                                 data->upload_font_buffer_mem, 0));

   /* Upload to Buffer */
   char* map = NULL;
   VK_CHECK(device_data->vtable.MapMemory(device_data->device,
                                          data->upload_font_buffer_mem,
                                          0, upload_size, 0, (void**)(&map)));
   memcpy(map, pixels, upload_size);
   VkMappedMemoryRange range[1] = {};
   range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
   range[0].memory = data->upload_font_buffer_mem;
   range[0].size = upload_size;
   VK_CHECK(device_data->vtable.FlushMappedMemoryRanges(device_data->device, 1, range));
   device_data->vtable.UnmapMemory(device_data->device,
                                   data->upload_font_buffer_mem);

   /* Copy buffer to image */
   VkImageMemoryBarrier copy_barrier[1] = {};
   copy_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   copy_barrier[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   copy_barrier[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   copy_barrier[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   copy_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   copy_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   copy_barrier[0].image = data->font_image;
   copy_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   copy_barrier[0].subresourceRange.levelCount = 1;
   copy_barrier[0].subresourceRange.layerCount = 1;
   device_data->vtable.CmdPipelineBarrier(command_buffer,
                                          VK_PIPELINE_STAGE_HOST_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          0, 0, NULL, 0, NULL,
                                          1, copy_barrier);

   VkBufferImageCopy region = {};
   region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   region.imageSubresource.layerCount = 1;
   region.imageExtent.width = width;
   region.imageExtent.height = height;
   region.imageExtent.depth = 1;
   device_data->vtable.CmdCopyBufferToImage(command_buffer,
                                            data->upload_font_buffer,
                                            data->font_image,
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            1, &region);

   VkImageMemoryBarrier use_barrier[1] = {};
   use_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   use_barrier[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   use_barrier[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
   use_barrier[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   use_barrier[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
   use_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   use_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   use_barrier[0].image = data->font_image;
   use_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   use_barrier[0].subresourceRange.levelCount = 1;
   use_barrier[0].subresourceRange.layerCount = 1;
   device_data->vtable.CmdPipelineBarrier(command_buffer,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                          0,
                                          0, NULL,
                                          0, NULL,
                                          1, use_barrier);

   /* Store our identifier */
   io.Fonts->TexID = (ImTextureID)(intptr_t)data->font_image;
}

static void CreateOrResizeBuffer(struct device_data *data,
                                 VkBuffer *buffer,
                                 VkDeviceMemory *buffer_memory,
                                 VkDeviceSize *buffer_size,
                                 size_t new_size, VkBufferUsageFlagBits usage)
{
    if (*buffer != VK_NULL_HANDLE)
        data->vtable.DestroyBuffer(data->device, *buffer, NULL);
    if (*buffer_memory)
        data->vtable.FreeMemory(data->device, *buffer_memory, NULL);

    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = new_size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(data->vtable.CreateBuffer(data->device, &buffer_info, NULL, buffer));

    VkMemoryRequirements req;
    data->vtable.GetBufferMemoryRequirements(data->device, *buffer, &req);
    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = req.size;
    alloc_info.memoryTypeIndex =
       vk_memory_type(data, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, req.memoryTypeBits);
    VK_CHECK(data->vtable.AllocateMemory(data->device, &alloc_info, NULL, buffer_memory));

    VK_CHECK(data->vtable.BindBufferMemory(data->device, *buffer, *buffer_memory, 0));
    *buffer_size = new_size;
}

static struct overlay_draw *render_swapchain_display(struct swapchain_data *data,
                                                     struct queue_data *present_queue,
                                                     const VkSemaphore *wait_semaphores,
                                                     unsigned n_wait_semaphores,
                                                     unsigned image_index)
{
   ImDrawData* draw_data = ImGui::GetDrawData();
   if (draw_data->TotalVtxCount == 0)
      return NULL;

   struct device_data *device_data = data->device;
   struct overlay_draw *draw = get_overlay_draw(data);

   device_data->vtable.ResetCommandBuffer(draw->command_buffer, 0);

   VkRenderPassBeginInfo render_pass_info = {};
   render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
   render_pass_info.renderPass = data->render_pass;
   render_pass_info.framebuffer = data->framebuffers[image_index];
   render_pass_info.renderArea.extent.width = data->width;
   render_pass_info.renderArea.extent.height = data->height;

   VkCommandBufferBeginInfo buffer_begin_info = {};
   buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

   device_data->vtable.BeginCommandBuffer(draw->command_buffer, &buffer_begin_info);

   ensure_swapchain_fonts(data, draw->command_buffer);

   /* Bounce the image to display back to color attachment layout for
    * rendering on top of it.
    */
   VkImageMemoryBarrier imb;
   imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   imb.pNext = nullptr;
   imb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   imb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   imb.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   imb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   imb.image = data->images[image_index];
   imb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   imb.subresourceRange.baseMipLevel = 0;
   imb.subresourceRange.levelCount = 1;
   imb.subresourceRange.baseArrayLayer = 0;
   imb.subresourceRange.layerCount = 1;
   imb.srcQueueFamilyIndex = present_queue->family_index;
   imb.dstQueueFamilyIndex = device_data->graphic_queue->family_index;
   device_data->vtable.CmdPipelineBarrier(draw->command_buffer,
                                          VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                                          VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                                          0,          /* dependency flags */
                                          0, nullptr, /* memory barriers */
                                          0, nullptr, /* buffer memory barriers */
                                          1, &imb);   /* image memory barriers */

   device_data->vtable.CmdBeginRenderPass(draw->command_buffer, &render_pass_info,
                                          VK_SUBPASS_CONTENTS_INLINE);

   /* Create/Resize vertex & index buffers */
   size_t vertex_size = draw_data->TotalVtxCount * sizeof(ImDrawVert);
   size_t index_size = draw_data->TotalIdxCount * sizeof(ImDrawIdx);
   if (draw->vertex_buffer_size < vertex_size) {
      CreateOrResizeBuffer(device_data,
                           &draw->vertex_buffer,
                           &draw->vertex_buffer_mem,
                           &draw->vertex_buffer_size,
                           vertex_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
   }
   if (draw->index_buffer_size < index_size) {
      CreateOrResizeBuffer(device_data,
                           &draw->index_buffer,
                           &draw->index_buffer_mem,
                           &draw->index_buffer_size,
                           index_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
   }

    /* Upload vertex & index data */
    ImDrawVert* vtx_dst = NULL;
    ImDrawIdx* idx_dst = NULL;
    VK_CHECK(device_data->vtable.MapMemory(device_data->device, draw->vertex_buffer_mem,
                                           0, vertex_size, 0, (void**)(&vtx_dst)));
    VK_CHECK(device_data->vtable.MapMemory(device_data->device, draw->index_buffer_mem,
                                           0, index_size, 0, (void**)(&idx_dst)));
    for (int n = 0; n < draw_data->CmdListsCount; n++)
        {
           const ImDrawList* cmd_list = draw_data->CmdLists[n];
           memcpy(vtx_dst, cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
           memcpy(idx_dst, cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx));
           vtx_dst += cmd_list->VtxBuffer.Size;
           idx_dst += cmd_list->IdxBuffer.Size;
        }
    VkMappedMemoryRange range[2] = {};
    range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range[0].memory = draw->vertex_buffer_mem;
    range[0].size = VK_WHOLE_SIZE;
    range[1].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range[1].memory = draw->index_buffer_mem;
    range[1].size = VK_WHOLE_SIZE;
    VK_CHECK(device_data->vtable.FlushMappedMemoryRanges(device_data->device, 2, range));
    device_data->vtable.UnmapMemory(device_data->device, draw->vertex_buffer_mem);
    device_data->vtable.UnmapMemory(device_data->device, draw->index_buffer_mem);

    /* Bind pipeline and descriptor sets */
    device_data->vtable.CmdBindPipeline(draw->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, data->pipeline);
    VkDescriptorSet desc_set[1] = { data->descriptor_set };
    device_data->vtable.CmdBindDescriptorSets(draw->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                              data->pipeline_layout, 0, 1, desc_set, 0, NULL);

    /* Bind vertex & index buffers */
    VkBuffer vertex_buffers[1] = { draw->vertex_buffer };
    VkDeviceSize vertex_offset[1] = { 0 };
    device_data->vtable.CmdBindVertexBuffers(draw->command_buffer, 0, 1, vertex_buffers, vertex_offset);
    device_data->vtable.CmdBindIndexBuffer(draw->command_buffer, draw->index_buffer, 0, VK_INDEX_TYPE_UINT16);

    /* Setup viewport */
    VkViewport viewport;
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = draw_data->DisplaySize.x;
    viewport.height = draw_data->DisplaySize.y;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    device_data->vtable.CmdSetViewport(draw->command_buffer, 0, 1, &viewport);


    /* Setup scale and translation through push constants :
     *
     * Our visible imgui space lies from draw_data->DisplayPos (top left) to
     * draw_data->DisplayPos+data_data->DisplaySize (bottom right). DisplayMin
     * is typically (0,0) for single viewport apps.
     */
    float scale[2];
    scale[0] = 2.0f / draw_data->DisplaySize.x;
    scale[1] = 2.0f / draw_data->DisplaySize.y;
    float translate[2];
    translate[0] = -1.0f - draw_data->DisplayPos.x * scale[0];
    translate[1] = -1.0f - draw_data->DisplayPos.y * scale[1];
    device_data->vtable.CmdPushConstants(draw->command_buffer, data->pipeline_layout,
                                         VK_SHADER_STAGE_VERTEX_BIT,
                                         sizeof(float) * 0, sizeof(float) * 2, scale);
    device_data->vtable.CmdPushConstants(draw->command_buffer, data->pipeline_layout,
                                         VK_SHADER_STAGE_VERTEX_BIT,
                                         sizeof(float) * 2, sizeof(float) * 2, translate);

    // Render the command lists:
    int vtx_offset = 0;
    int idx_offset = 0;
    ImVec2 display_pos = draw_data->DisplayPos;
    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
            // Apply scissor/clipping rectangle
            // FIXME: We could clamp width/height based on clamped min/max values.
            VkRect2D scissor;
            scissor.offset.x = (int32_t)(pcmd->ClipRect.x - display_pos.x) > 0 ? (int32_t)(pcmd->ClipRect.x - display_pos.x) : 0;
            scissor.offset.y = (int32_t)(pcmd->ClipRect.y - display_pos.y) > 0 ? (int32_t)(pcmd->ClipRect.y - display_pos.y) : 0;
            scissor.extent.width = (uint32_t)(pcmd->ClipRect.z - pcmd->ClipRect.x);
            scissor.extent.height = (uint32_t)(pcmd->ClipRect.w - pcmd->ClipRect.y + 1); // FIXME: Why +1 here?
            device_data->vtable.CmdSetScissor(draw->command_buffer, 0, 1, &scissor);

            // Draw
            device_data->vtable.CmdDrawIndexed(draw->command_buffer, pcmd->ElemCount, 1, idx_offset, vtx_offset, 0);

            idx_offset += pcmd->ElemCount;
        }
        vtx_offset += cmd_list->VtxBuffer.Size;
    }

   device_data->vtable.CmdEndRenderPass(draw->command_buffer);

   if (device_data->graphic_queue->family_index != present_queue->family_index)
   {
      /* Transfer the image back to the present queue family
       * image layout was already changed to present by the render pass
       */
      imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      imb.pNext = nullptr;
      imb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      imb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      imb.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      imb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      imb.image = data->images[image_index];
      imb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      imb.subresourceRange.baseMipLevel = 0;
      imb.subresourceRange.levelCount = 1;
      imb.subresourceRange.baseArrayLayer = 0;
      imb.subresourceRange.layerCount = 1;
      imb.srcQueueFamilyIndex = device_data->graphic_queue->family_index;
      imb.dstQueueFamilyIndex = present_queue->family_index;
      device_data->vtable.CmdPipelineBarrier(draw->command_buffer,
                                             VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                                             VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                                             0,          /* dependency flags */
                                             0, nullptr, /* memory barriers */
                                             0, nullptr, /* buffer memory barriers */
                                             1, &imb);   /* image memory barriers */
   }

   device_data->vtable.EndCommandBuffer(draw->command_buffer);

   /* When presenting on a different queue than where we're drawing the
    * overlay *AND* when the application does not provide a semaphore to
    * vkQueuePresent, insert our own cross engine synchronization
    * semaphore.
    */
   if (n_wait_semaphores == 0 && device_data->graphic_queue->queue != present_queue->queue) {
      VkPipelineStageFlags stages_wait = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      VkSubmitInfo submit_info = {};
      submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit_info.commandBufferCount = 0;
      submit_info.pWaitDstStageMask = &stages_wait;
      submit_info.waitSemaphoreCount = 0;
      submit_info.signalSemaphoreCount = 1;
      submit_info.pSignalSemaphores = &draw->cross_engine_semaphore;

      device_data->vtable.QueueSubmit(present_queue->queue, 1, &submit_info, VK_NULL_HANDLE);

      submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit_info.commandBufferCount = 1;
      submit_info.pWaitDstStageMask = &stages_wait;
      submit_info.pCommandBuffers = &draw->command_buffer;
      submit_info.waitSemaphoreCount = 1;
      submit_info.pWaitSemaphores = &draw->cross_engine_semaphore;
      submit_info.signalSemaphoreCount = 1;
      submit_info.pSignalSemaphores = &draw->semaphore;

      device_data->vtable.QueueSubmit(device_data->graphic_queue->queue, 1, &submit_info, draw->fence);
   } else {
      // wait in the fragment stage until the swapchain image is ready
      std::vector<VkPipelineStageFlags> stages_wait(n_wait_semaphores, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

      VkSubmitInfo submit_info = {};
      submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit_info.commandBufferCount = 1;
      submit_info.pCommandBuffers = &draw->command_buffer;
      submit_info.pWaitDstStageMask = stages_wait.data();
      submit_info.waitSemaphoreCount = n_wait_semaphores;
      submit_info.pWaitSemaphores = wait_semaphores;
      submit_info.signalSemaphoreCount = 1;
      submit_info.pSignalSemaphores = &draw->semaphore;

      device_data->vtable.QueueSubmit(device_data->graphic_queue->queue, 1, &submit_info, draw->fence);
   }

   return draw;
}

static const uint32_t overlay_vert_spv[] = {
#include "overlay.vert.spv.h"
};
static const uint32_t overlay_frag_spv[] = {
#include "overlay.frag.spv.h"
};

static void setup_swapchain_data_pipeline(struct swapchain_data *data)
{
   struct device_data *device_data = data->device;
   VkShaderModule vert_module, frag_module;

   /* Create shader modules */
   VkShaderModuleCreateInfo vert_info = {};
   vert_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
   vert_info.codeSize = sizeof(overlay_vert_spv);
   vert_info.pCode = overlay_vert_spv;
   VK_CHECK(device_data->vtable.CreateShaderModule(device_data->device,
                                                   &vert_info, NULL, &vert_module));
   VkShaderModuleCreateInfo frag_info = {};
   frag_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
   frag_info.codeSize = sizeof(overlay_frag_spv);
   frag_info.pCode = (uint32_t*)overlay_frag_spv;
   VK_CHECK(device_data->vtable.CreateShaderModule(device_data->device,
                                                   &frag_info, NULL, &frag_module));

   /* Font sampler */
   VkSamplerCreateInfo sampler_info = {};
   sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
   sampler_info.magFilter = VK_FILTER_LINEAR;
   sampler_info.minFilter = VK_FILTER_LINEAR;
   sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
   sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
   sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
   sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
   sampler_info.minLod = -1000;
   sampler_info.maxLod = 1000;
   sampler_info.maxAnisotropy = 1.0f;
   VK_CHECK(device_data->vtable.CreateSampler(device_data->device, &sampler_info,
                                              NULL, &data->font_sampler));

   /* Descriptor pool */
   VkDescriptorPoolSize sampler_pool_size = {};
   sampler_pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   sampler_pool_size.descriptorCount = 1;
   VkDescriptorPoolCreateInfo desc_pool_info = {};
   desc_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
   desc_pool_info.maxSets = 1;
   desc_pool_info.poolSizeCount = 1;
   desc_pool_info.pPoolSizes = &sampler_pool_size;
   VK_CHECK(device_data->vtable.CreateDescriptorPool(device_data->device,
                                                     &desc_pool_info,
                                                     NULL, &data->descriptor_pool));

   /* Descriptor layout */
   VkSampler sampler[1] = { data->font_sampler };
   VkDescriptorSetLayoutBinding binding[1] = {};
   binding[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   binding[0].descriptorCount = 1;
   binding[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
   binding[0].pImmutableSamplers = sampler;
   VkDescriptorSetLayoutCreateInfo set_layout_info = {};
   set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
   set_layout_info.bindingCount = 1;
   set_layout_info.pBindings = binding;
   VK_CHECK(device_data->vtable.CreateDescriptorSetLayout(device_data->device,
                                                          &set_layout_info,
                                                          NULL, &data->descriptor_layout));

   /* Descriptor set */
   VkDescriptorSetAllocateInfo alloc_info = {};
   alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
   alloc_info.descriptorPool = data->descriptor_pool;
   alloc_info.descriptorSetCount = 1;
   alloc_info.pSetLayouts = &data->descriptor_layout;
   VK_CHECK(device_data->vtable.AllocateDescriptorSets(device_data->device,
                                                       &alloc_info,
                                                       &data->descriptor_set));

   /* Constants: we are using 'vec2 offset' and 'vec2 scale' instead of a full
    * 3d projection matrix
    */
   VkPushConstantRange push_constants[1] = {};
   push_constants[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
   push_constants[0].offset = sizeof(float) * 0;
   push_constants[0].size = sizeof(float) * 4;
   VkPipelineLayoutCreateInfo layout_info = {};
   layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
   layout_info.setLayoutCount = 1;
   layout_info.pSetLayouts = &data->descriptor_layout;
   layout_info.pushConstantRangeCount = 1;
   layout_info.pPushConstantRanges = push_constants;
   VK_CHECK(device_data->vtable.CreatePipelineLayout(device_data->device,
                                                     &layout_info,
                                                     NULL, &data->pipeline_layout));

   VkPipelineShaderStageCreateInfo stage[2] = {};
   stage[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
   stage[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
   stage[0].module = vert_module;
   stage[0].pName = "main";
   stage[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
   stage[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
   stage[1].module = frag_module;
   stage[1].pName = "main";

   VkVertexInputBindingDescription binding_desc[1] = {};
   binding_desc[0].stride = sizeof(ImDrawVert);
   binding_desc[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

   VkVertexInputAttributeDescription attribute_desc[3] = {};
   attribute_desc[0].location = 0;
   attribute_desc[0].binding = binding_desc[0].binding;
   attribute_desc[0].format = VK_FORMAT_R32G32_SFLOAT;
   attribute_desc[0].offset = IM_OFFSETOF(ImDrawVert, pos);
   attribute_desc[1].location = 1;
   attribute_desc[1].binding = binding_desc[0].binding;
   attribute_desc[1].format = VK_FORMAT_R32G32_SFLOAT;
   attribute_desc[1].offset = IM_OFFSETOF(ImDrawVert, uv);
   attribute_desc[2].location = 2;
   attribute_desc[2].binding = binding_desc[0].binding;
   attribute_desc[2].format = VK_FORMAT_R8G8B8A8_UNORM;
   attribute_desc[2].offset = IM_OFFSETOF(ImDrawVert, col);

   VkPipelineVertexInputStateCreateInfo vertex_info = {};
   vertex_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
   vertex_info.vertexBindingDescriptionCount = 1;
   vertex_info.pVertexBindingDescriptions = binding_desc;
   vertex_info.vertexAttributeDescriptionCount = 3;
   vertex_info.pVertexAttributeDescriptions = attribute_desc;

   VkPipelineInputAssemblyStateCreateInfo ia_info = {};
   ia_info.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
   ia_info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

   VkPipelineViewportStateCreateInfo viewport_info = {};
   viewport_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
   viewport_info.viewportCount = 1;
   viewport_info.scissorCount = 1;

   VkPipelineRasterizationStateCreateInfo raster_info = {};
   raster_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
   raster_info.polygonMode = VK_POLYGON_MODE_FILL;
   raster_info.cullMode = VK_CULL_MODE_NONE;
   raster_info.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
   raster_info.lineWidth = 1.0f;

   VkPipelineMultisampleStateCreateInfo ms_info = {};
   ms_info.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
   ms_info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

   VkPipelineColorBlendAttachmentState color_attachment[1] = {};
   color_attachment[0].blendEnable = VK_TRUE;
   color_attachment[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
   color_attachment[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
   color_attachment[0].colorBlendOp = VK_BLEND_OP_ADD;
   color_attachment[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
   color_attachment[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
   color_attachment[0].alphaBlendOp = VK_BLEND_OP_ADD;
   color_attachment[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
      VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

   VkPipelineDepthStencilStateCreateInfo depth_info = {};
   depth_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

   VkPipelineColorBlendStateCreateInfo blend_info = {};
   blend_info.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
   blend_info.attachmentCount = 1;
   blend_info.pAttachments = color_attachment;

   VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
   VkPipelineDynamicStateCreateInfo dynamic_state = {};
   dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
   dynamic_state.dynamicStateCount = (uint32_t)IM_ARRAYSIZE(dynamic_states);
   dynamic_state.pDynamicStates = dynamic_states;

   VkGraphicsPipelineCreateInfo info = {};
   info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
   info.flags = 0;
   info.stageCount = 2;
   info.pStages = stage;
   info.pVertexInputState = &vertex_info;
   info.pInputAssemblyState = &ia_info;
   info.pViewportState = &viewport_info;
   info.pRasterizationState = &raster_info;
   info.pMultisampleState = &ms_info;
   info.pDepthStencilState = &depth_info;
   info.pColorBlendState = &blend_info;
   info.pDynamicState = &dynamic_state;
   info.layout = data->pipeline_layout;
   info.renderPass = data->render_pass;
   VK_CHECK(
      device_data->vtable.CreateGraphicsPipelines(device_data->device, VK_NULL_HANDLE,
                                                  1, &info,
                                                  NULL, &data->pipeline));

   device_data->vtable.DestroyShaderModule(device_data->device, vert_module, NULL);
   device_data->vtable.DestroyShaderModule(device_data->device, frag_module, NULL);

   ImGuiIO& io = ImGui::GetIO();
   int font_size = device_data->instance->params.font_size;
   if (!font_size)
      font_size = 24;

   // ImGui takes ownership of the data, no need to free it
   if (!device_data->instance->params.font_file.empty() && file_exists(device_data->instance->params.font_file)) {
      data->font = io.Fonts->AddFontFromFileTTF(device_data->instance->params.font_file.c_str(), font_size);
      data->sw_stats.font1 = io.Fonts->AddFontFromFileTTF(device_data->instance->params.font_file.c_str(), font_size * 0.55f);
   } else {
      ImFontConfig font_cfg = ImFontConfig();
      const char* ttf_compressed_base85 = GetDefaultCompressedFontDataTTFBase85();
      const ImWchar* glyph_ranges = io.Fonts->GetGlyphRangesDefault();

      data->font = io.Fonts->AddFontFromMemoryCompressedBase85TTF(ttf_compressed_base85, font_size, &font_cfg, glyph_ranges);
      data->sw_stats.font1 = io.Fonts->AddFontFromMemoryCompressedBase85TTF(ttf_compressed_base85, font_size * 0.55, &font_cfg, glyph_ranges);
   }
   unsigned char* pixels;
   int width, height;
   io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

   /* Font image */
   VkImageCreateInfo image_info = {};
   image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
   image_info.imageType = VK_IMAGE_TYPE_2D;
   image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
   image_info.extent.width = width;
   image_info.extent.height = height;
   image_info.extent.depth = 1;
   image_info.mipLevels = 1;
   image_info.arrayLayers = 1;
   image_info.samples = VK_SAMPLE_COUNT_1_BIT;
   image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
   image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   VK_CHECK(device_data->vtable.CreateImage(device_data->device, &image_info,
                                            NULL, &data->font_image));
   VkMemoryRequirements font_image_req;
   device_data->vtable.GetImageMemoryRequirements(device_data->device,
                                                  data->font_image, &font_image_req);
   VkMemoryAllocateInfo image_alloc_info = {};
   image_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   image_alloc_info.allocationSize = font_image_req.size;
   image_alloc_info.memoryTypeIndex = vk_memory_type(device_data,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                     font_image_req.memoryTypeBits);
   VK_CHECK(device_data->vtable.AllocateMemory(device_data->device, &image_alloc_info,
                                               NULL, &data->font_mem));
   VK_CHECK(device_data->vtable.BindImageMemory(device_data->device,
                                                data->font_image,
                                                data->font_mem, 0));

   /* Font image view */
   VkImageViewCreateInfo view_info = {};
   view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
   view_info.image = data->font_image;
   view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
   view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
   view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   view_info.subresourceRange.levelCount = 1;
   view_info.subresourceRange.layerCount = 1;
   VK_CHECK(device_data->vtable.CreateImageView(device_data->device, &view_info,
                                                NULL, &data->font_image_view));

   /* Descriptor set */
   VkDescriptorImageInfo desc_image[1] = {};
   desc_image[0].sampler = data->font_sampler;
   desc_image[0].imageView = data->font_image_view;
   desc_image[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
   VkWriteDescriptorSet write_desc[1] = {};
   write_desc[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
   write_desc[0].dstSet = data->descriptor_set;
   write_desc[0].descriptorCount = 1;
   write_desc[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   write_desc[0].pImageInfo = desc_image;
   device_data->vtable.UpdateDescriptorSets(device_data->device, 1, write_desc, 0, NULL);
}

void imgui_custom_style(struct overlay_params& params){
   ImGuiStyle& style = ImGui::GetStyle();
   style.Colors[ImGuiCol_PlotLines] = ImGui::ColorConvertU32ToFloat4(params.frametime_color);
   style.Colors[ImGuiCol_WindowBg]  = ImGui::ColorConvertU32ToFloat4(params.background_color);
   style.Colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(params.text_color);
   style.CellPadding.y = -2;
}

static void setup_swapchain_data(struct swapchain_data *data,
                                 const VkSwapchainCreateInfoKHR *pCreateInfo, struct overlay_params& params)
{
   data->width = pCreateInfo->imageExtent.width;
   data->height = pCreateInfo->imageExtent.height;
   data->format = pCreateInfo->imageFormat;

   data->imgui_context = ImGui::CreateContext();
   ImGui::SetCurrentContext(data->imgui_context);

   ImGui::GetIO().IniFilename = NULL;
   ImGui::GetIO().DisplaySize = ImVec2((float)data->width, (float)data->height);
   imgui_custom_style(params);

   struct device_data *device_data = data->device;

   /* Render pass */
   VkAttachmentDescription attachment_desc = {};
   attachment_desc.format = pCreateInfo->imageFormat;
   attachment_desc.samples = VK_SAMPLE_COUNT_1_BIT;
   attachment_desc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
   attachment_desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   attachment_desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   attachment_desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   attachment_desc.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   attachment_desc.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   VkAttachmentReference color_attachment = {};
   color_attachment.attachment = 0;
   color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   VkSubpassDescription subpass = {};
   subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   subpass.colorAttachmentCount = 1;
   subpass.pColorAttachments = &color_attachment;
   VkSubpassDependency dependency = {};
   dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
   dependency.dstSubpass = 0;
   dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
   dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
   dependency.srcAccessMask = 0;
   dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   VkRenderPassCreateInfo render_pass_info = {};
   render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
   render_pass_info.attachmentCount = 1;
   render_pass_info.pAttachments = &attachment_desc;
   render_pass_info.subpassCount = 1;
   render_pass_info.pSubpasses = &subpass;
   render_pass_info.dependencyCount = 1;
   render_pass_info.pDependencies = &dependency;
   VK_CHECK(device_data->vtable.CreateRenderPass(device_data->device,
                                                 &render_pass_info,
                                                 NULL, &data->render_pass));

   setup_swapchain_data_pipeline(data);

   uint32_t n_images = 0;
   VK_CHECK(device_data->vtable.GetSwapchainImagesKHR(device_data->device,
                                                      data->swapchain,
                                                      &n_images,
                                                      NULL));

   data->images.resize(n_images);
   data->image_views.resize(n_images);
   data->framebuffers.resize(n_images);

   VK_CHECK(device_data->vtable.GetSwapchainImagesKHR(device_data->device,
                                                      data->swapchain,
                                                      &n_images,
                                                      data->images.data()));


   if (n_images != data->images.size()) {
      data->images.resize(n_images);
      data->image_views.resize(n_images);
      data->framebuffers.resize(n_images);
   }

   /* Image views */
   VkImageViewCreateInfo view_info = {};
   view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
   view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
   view_info.format = pCreateInfo->imageFormat;
   view_info.components.r = VK_COMPONENT_SWIZZLE_R;
   view_info.components.g = VK_COMPONENT_SWIZZLE_G;
   view_info.components.b = VK_COMPONENT_SWIZZLE_B;
   view_info.components.a = VK_COMPONENT_SWIZZLE_A;
   view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   for (size_t i = 0; i < data->images.size(); i++) {
      view_info.image = data->images[i];
      VK_CHECK(device_data->vtable.CreateImageView(device_data->device,
                                                   &view_info, NULL,
                                                   &data->image_views[i]));
   }

   /* Framebuffers */
   VkImageView attachment[1];
   VkFramebufferCreateInfo fb_info = {};
   fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
   fb_info.renderPass = data->render_pass;
   fb_info.attachmentCount = 1;
   fb_info.pAttachments = attachment;
   fb_info.width = data->width;
   fb_info.height = data->height;
   fb_info.layers = 1;
   for (size_t i = 0; i < data->image_views.size(); i++) {
      attachment[0] = data->image_views[i];
      VK_CHECK(device_data->vtable.CreateFramebuffer(device_data->device, &fb_info,
                                                     NULL, &data->framebuffers[i]));
   }

   /* Command buffer pool */
   VkCommandPoolCreateInfo cmd_buffer_pool_info = {};
   cmd_buffer_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   cmd_buffer_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   cmd_buffer_pool_info.queueFamilyIndex = device_data->graphic_queue->family_index;
   VK_CHECK(device_data->vtable.CreateCommandPool(device_data->device,
                                                  &cmd_buffer_pool_info,
                                                  NULL, &data->command_pool));
}

static void shutdown_swapchain_data(struct swapchain_data *data)
{
   struct device_data *device_data = data->device;

   for (auto draw : data->draws) {
      device_data->vtable.DestroySemaphore(device_data->device, draw->cross_engine_semaphore, NULL);
      device_data->vtable.DestroySemaphore(device_data->device, draw->semaphore, NULL);
      device_data->vtable.DestroyFence(device_data->device, draw->fence, NULL);
      device_data->vtable.DestroyBuffer(device_data->device, draw->vertex_buffer, NULL);
      device_data->vtable.DestroyBuffer(device_data->device, draw->index_buffer, NULL);
      device_data->vtable.FreeMemory(device_data->device, draw->vertex_buffer_mem, NULL);
      device_data->vtable.FreeMemory(device_data->device, draw->index_buffer_mem, NULL);
      delete draw;
   }

   for (size_t i = 0; i < data->images.size(); i++) {
      device_data->vtable.DestroyImageView(device_data->device, data->image_views[i], NULL);
      device_data->vtable.DestroyFramebuffer(device_data->device, data->framebuffers[i], NULL);
   }

   device_data->vtable.DestroyRenderPass(device_data->device, data->render_pass, NULL);

   device_data->vtable.DestroyCommandPool(device_data->device, data->command_pool, NULL);

   device_data->vtable.DestroyPipeline(device_data->device, data->pipeline, NULL);
   device_data->vtable.DestroyPipelineLayout(device_data->device, data->pipeline_layout, NULL);

   device_data->vtable.DestroyDescriptorPool(device_data->device,
                                             data->descriptor_pool, NULL);
   device_data->vtable.DestroyDescriptorSetLayout(device_data->device,
                                                  data->descriptor_layout, NULL);

   device_data->vtable.DestroySampler(device_data->device, data->font_sampler, NULL);
   device_data->vtable.DestroyImageView(device_data->device, data->font_image_view, NULL);
   device_data->vtable.DestroyImage(device_data->device, data->font_image, NULL);
   device_data->vtable.FreeMemory(device_data->device, data->font_mem, NULL);

   device_data->vtable.DestroyBuffer(device_data->device, data->upload_font_buffer, NULL);
   device_data->vtable.FreeMemory(device_data->device, data->upload_font_buffer_mem, NULL);

   ImGui::DestroyContext(data->imgui_context);
}

static struct overlay_draw *before_present(struct swapchain_data *swapchain_data,
                                           struct queue_data *present_queue,
                                           const VkSemaphore *wait_semaphores,
                                           unsigned n_wait_semaphores,
                                           unsigned imageIndex)
{
   struct overlay_draw *draw = NULL;

   snapshot_swapchain_frame(swapchain_data);

   if (swapchain_data->sw_stats.n_frames > 0) {
      compute_swapchain_display(swapchain_data);
      draw = render_swapchain_display(swapchain_data, present_queue,
                                      wait_semaphores, n_wait_semaphores,
                                      imageIndex);
   }

   return draw;
}

static VkResult overlay_CreateSwapchainKHR(
    VkDevice                                    device,
    const VkSwapchainCreateInfoKHR*             pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSwapchainKHR*                             pSwapchain)
{
   struct device_data *device_data = FIND(struct device_data, device);
   array<VkPresentModeKHR, 4> modes = {VK_PRESENT_MODE_FIFO_RELAXED_KHR,
           VK_PRESENT_MODE_IMMEDIATE_KHR,
           VK_PRESENT_MODE_MAILBOX_KHR,
           VK_PRESENT_MODE_FIFO_KHR};

   if (device_data->instance->params.vsync < 4)
      const_cast<VkSwapchainCreateInfoKHR*> (pCreateInfo)->presentMode = modes[device_data->instance->params.vsync];

   VkResult result = device_data->vtable.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
   if (result != VK_SUCCESS) return result;
   struct swapchain_data *swapchain_data = new_swapchain_data(*pSwapchain, device_data);
   setup_swapchain_data(swapchain_data, pCreateInfo, device_data->instance->params);

   const VkPhysicalDeviceProperties& prop = device_data->properties;
   swapchain_data->sw_stats.version_vk.major = VK_VERSION_MAJOR(prop.apiVersion);
   swapchain_data->sw_stats.version_vk.minor = VK_VERSION_MINOR(prop.apiVersion);
   swapchain_data->sw_stats.version_vk.patch = VK_VERSION_PATCH(prop.apiVersion);
   swapchain_data->sw_stats.engineName    = device_data->instance->engineName;
   swapchain_data->sw_stats.engineVersion = device_data->instance->engineVersion;

   std::stringstream ss;
   ss << prop.deviceName;
   if (prop.vendorID == 0x10de) {
      ss << " (" << ((prop.driverVersion >> 22) & 0x3ff);
      ss << "."  << ((prop.driverVersion >> 14) & 0x0ff);
      ss << "."  << std::setw(2) << std::setfill('0') << ((prop.driverVersion >> 6) & 0x0ff);
#ifdef _WIN32
   } else if (prop.vendorID == 0x8086) {
      ss << " (" << (prop.driverVersion >> 14);
      ss << "."  << (prop.driverVersion & 0x3fff);
   }
#endif
   } else {
      ss << " (" << VK_VERSION_MAJOR(prop.driverVersion);
      ss << "."  << VK_VERSION_MINOR(prop.driverVersion);
      ss << "."  << VK_VERSION_PATCH(prop.driverVersion);
   }
   ss << ")";
   swapchain_data->sw_stats.deviceName = ss.str();

   return result;
}

static void overlay_DestroySwapchainKHR(
    VkDevice                                    device,
    VkSwapchainKHR                              swapchain,
    const VkAllocationCallbacks*                pAllocator)
{
   struct swapchain_data *swapchain_data =
      FIND(struct swapchain_data, swapchain);

   shutdown_swapchain_data(swapchain_data);
   swapchain_data->device->vtable.DestroySwapchainKHR(device, swapchain, pAllocator);
   destroy_swapchain_data(swapchain_data);
}

void FpsLimiter(struct fps_limit& stats){
   stats.sleepTime = stats.targetFrameTime - (stats.frameStart - stats.frameEnd);
   if (stats.sleepTime > stats.frameOverhead) {
      int64_t adjustedSleep = stats.sleepTime - stats.frameOverhead;
      this_thread::sleep_for(chrono::nanoseconds(adjustedSleep));
      stats.frameOverhead = ((os_time_get_nano() - stats.frameStart) - adjustedSleep);
      if (stats.frameOverhead > stats.targetFrameTime)
         stats.frameOverhead = 0;
   }
}

static VkResult overlay_QueuePresentKHR(
    VkQueue                                     queue,
    const VkPresentInfoKHR*                     pPresentInfo)
{
   struct queue_data *queue_data = FIND(struct queue_data, queue);

   /* Otherwise we need to add our overlay drawing semaphore to the list of
    * semaphores to wait on. If we don't do that the presented picture might
    * be have incomplete overlay drawings.
    */
   VkResult result = VK_SUCCESS;
   for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
      VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[i];
      struct swapchain_data *swapchain_data =
         FIND(struct swapchain_data, swapchain);

      uint32_t image_index = pPresentInfo->pImageIndices[i];

      VkPresentInfoKHR present_info = *pPresentInfo;
      present_info.swapchainCount = 1;
      present_info.pSwapchains = &swapchain;
      present_info.pImageIndices = &image_index;

      struct overlay_draw *draw = before_present(swapchain_data,
                                                   queue_data,
                                                   pPresentInfo->pWaitSemaphores,
                                                   pPresentInfo->waitSemaphoreCount,
                                                   image_index);

      /* Because the submission of the overlay draw waits on the semaphores
         * handed for present, we don't need to have this present operation
         * wait on them as well, we can just wait on the overlay submission
         * semaphore.
         */
      if (draw) {
         present_info.pWaitSemaphores = &draw->semaphore;
         present_info.waitSemaphoreCount = 1;
      }

      VkResult chain_result = queue_data->device->vtable.QueuePresentKHR(queue, &present_info);
      if (pPresentInfo->pResults)
         pPresentInfo->pResults[i] = chain_result;
      if (chain_result != VK_SUCCESS && result == VK_SUCCESS)
         result = chain_result;
   }

   if (fps_limit_stats.targetFrameTime > 0){
      fps_limit_stats.frameStart = os_time_get_nano();
      FpsLimiter(fps_limit_stats);
      fps_limit_stats.frameEnd = os_time_get_nano();
   }
   
   return result;
}

static VkResult overlay_BeginCommandBuffer(
    VkCommandBuffer                             commandBuffer,
    const VkCommandBufferBeginInfo*             pBeginInfo)
{
   struct command_buffer_data *cmd_buffer_data =
      FIND(struct command_buffer_data, commandBuffer);
   struct device_data *device_data = cmd_buffer_data->device;

   /* Otherwise record a begin query as first command. */
   VkResult result = device_data->vtable.BeginCommandBuffer(commandBuffer, pBeginInfo);

   return result;
}

static VkResult overlay_EndCommandBuffer(
    VkCommandBuffer                             commandBuffer)
{
   struct command_buffer_data *cmd_buffer_data =
      FIND(struct command_buffer_data, commandBuffer);
   struct device_data *device_data = cmd_buffer_data->device;

   return device_data->vtable.EndCommandBuffer(commandBuffer);
}

static VkResult overlay_ResetCommandBuffer(
    VkCommandBuffer                             commandBuffer,
    VkCommandBufferResetFlags                   flags)
{
   struct command_buffer_data *cmd_buffer_data =
      FIND(struct command_buffer_data, commandBuffer);
   struct device_data *device_data = cmd_buffer_data->device;

   return device_data->vtable.ResetCommandBuffer(commandBuffer, flags);
}

static void overlay_CmdExecuteCommands(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    commandBufferCount,
    const VkCommandBuffer*                      pCommandBuffers)
{
   struct command_buffer_data *cmd_buffer_data =
      FIND(struct command_buffer_data, commandBuffer);
   struct device_data *device_data = cmd_buffer_data->device;

   device_data->vtable.CmdExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers);
}

static VkResult overlay_AllocateCommandBuffers(
   VkDevice                           device,
   const VkCommandBufferAllocateInfo* pAllocateInfo,
   VkCommandBuffer*                   pCommandBuffers)
{
   struct device_data *device_data = FIND(struct device_data, device);
   VkResult result =
      device_data->vtable.AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
   if (result != VK_SUCCESS)
      return result;

   for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
      new_command_buffer_data(pCommandBuffers[i], pAllocateInfo->level,
                              device_data);
   }

   return result;
}

static void overlay_FreeCommandBuffers(
   VkDevice               device,
   VkCommandPool          commandPool,
   uint32_t               commandBufferCount,
   const VkCommandBuffer* pCommandBuffers)
{
   struct device_data *device_data = FIND(struct device_data, device);
   for (uint32_t i = 0; i < commandBufferCount; i++) {
      struct command_buffer_data *cmd_buffer_data =
         FIND(struct command_buffer_data, pCommandBuffers[i]);

      /* It is legal to free a NULL command buffer*/
      if (!cmd_buffer_data)
         continue;

      destroy_command_buffer_data(cmd_buffer_data);
   }

   device_data->vtable.FreeCommandBuffers(device, commandPool,
                                          commandBufferCount, pCommandBuffers);
}

static VkResult overlay_QueueSubmit(
    VkQueue                                     queue,
    uint32_t                                    submitCount,
    const VkSubmitInfo*                         pSubmits,
    VkFence                                     fence)
{
   struct queue_data *queue_data = FIND(struct queue_data, queue);
   struct device_data *device_data = queue_data->device;

   return device_data->vtable.QueueSubmit(queue, submitCount, pSubmits, fence);
}

static VkResult overlay_CreateDevice(
    VkPhysicalDevice                            physicalDevice,
    const VkDeviceCreateInfo*                   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDevice*                                   pDevice)
{
   struct instance_data *instance_data =
      FIND(struct instance_data, physicalDevice);
   VkLayerDeviceCreateInfo *chain_info =
      get_device_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

   assert(chain_info->u.pLayerInfo);
   PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
   PFN_vkCreateDevice fpCreateDevice = (PFN_vkCreateDevice)fpGetInstanceProcAddr(NULL, "vkCreateDevice");
   if (fpCreateDevice == NULL) {
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   // Advance the link info for the next element on the chain
   chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

   VkPhysicalDeviceFeatures device_features = {};
   VkDeviceCreateInfo device_info = *pCreateInfo;

   if (pCreateInfo->pEnabledFeatures)
      device_features = *(pCreateInfo->pEnabledFeatures);
   device_info.pEnabledFeatures = &device_features;


   VkResult result = fpCreateDevice(physicalDevice, &device_info, pAllocator, pDevice);
   if (result != VK_SUCCESS) return result;

   struct device_data *device_data = new_device_data(*pDevice, instance_data);
   device_data->physical_device = physicalDevice;
   vk_load_device_commands(*pDevice, fpGetDeviceProcAddr, &device_data->vtable);

   instance_data->vtable.GetPhysicalDeviceProperties(device_data->physical_device,
                                                     &device_data->properties);

   VkLayerDeviceCreateInfo *load_data_info =
      get_device_chain_info(pCreateInfo, VK_LOADER_DATA_CALLBACK);
   device_data->set_device_loader_data = load_data_info->u.pfnSetDeviceLoaderData;

   if (!is_blacklisted()) {
      device_map_queues(device_data, pCreateInfo);

      init_gpu_stats(device_data->properties.vendorID, instance_data->params);
      init_system_info();
   }

   return result;
}

static void overlay_DestroyDevice(
    VkDevice                                    device,
    const VkAllocationCallbacks*                pAllocator)
{
   struct device_data *device_data = FIND(struct device_data, device);
   if (!is_blacklisted())
      device_unmap_queues(device_data);
   device_data->vtable.DestroyDevice(device, pAllocator);
   destroy_device_data(device_data);
}

static VkResult overlay_CreateInstance(
    const VkInstanceCreateInfo*                 pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkInstance*                                 pInstance)
{
   VkLayerInstanceCreateInfo *chain_info =
      get_instance_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
      

   std::string engineName, engineVersion;
   if (!is_blacklisted()) {
      const char* pEngineName = nullptr;
      if (pCreateInfo->pApplicationInfo)
         pEngineName = pCreateInfo->pApplicationInfo->pEngineName;
      if (pEngineName)
         engineName = pEngineName;
      if (engineName == "DXVK" || engineName == "vkd3d") {
         int engineVer = pCreateInfo->pApplicationInfo->engineVersion;
         engineVersion = to_string(VK_VERSION_MAJOR(engineVer)) + "." + to_string(VK_VERSION_MINOR(engineVer)) + "." + to_string(VK_VERSION_PATCH(engineVer));
      }

      if (engineName != "DXVK" && engineName != "vkd3d" && engineName != "Feral3D")
         engineName = "VULKAN";

      if (engineName == "vkd3d")
         engineName = "VKD3D";
   }

   assert(chain_info->u.pLayerInfo);
   PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr =
      chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   PFN_vkCreateInstance fpCreateInstance =
      (PFN_vkCreateInstance)fpGetInstanceProcAddr(NULL, "vkCreateInstance");
   if (fpCreateInstance == NULL) {
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   // Advance the link info for the next element on the chain
   chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

   VkResult result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
   if (result != VK_SUCCESS) return result;

   struct instance_data *instance_data = new_instance_data(*pInstance);
   vk_load_instance_commands(instance_data->instance,
                             fpGetInstanceProcAddr,
                             &instance_data->vtable);
   instance_data_map_physical_devices(instance_data, true);

   if (!is_blacklisted()) {
      parse_overlay_config(&instance_data->params, getenv("MANGOHUD_CONFIG"));
      instance_data->notifier.params = &instance_data->params;
      start_notifier(instance_data->notifier);

      init_cpu_stats(instance_data->params);

      // Adjust height for DXVK/VKD3D version number
      if (engineName == "DXVK" || engineName == "VKD3D"){
         if (instance_data->params.font_size){
            instance_data->params.height += instance_data->params.font_size / 2;
         } else {
            instance_data->params.height += 24 / 2;
         }
      }

      instance_data->engineName = engineName;
      instance_data->engineVersion = engineVersion;
   }

   return result;
}

static void overlay_DestroyInstance(
    VkInstance                                  instance,
    const VkAllocationCallbacks*                pAllocator)
{
   struct instance_data *instance_data = FIND(struct instance_data, instance);
   instance_data_map_physical_devices(instance_data, false);
   instance_data->vtable.DestroyInstance(instance, pAllocator);
   if (!is_blacklisted())
      stop_notifier(instance_data->notifier);
   destroy_instance_data(instance_data);
}

extern "C" VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL overlay_GetDeviceProcAddr(VkDevice dev,
                                                                             const char *funcName);
static const struct {
   const char *name;
   void *ptr;
} name_to_funcptr_map[] = {
   { "vkGetDeviceProcAddr", (void *) overlay_GetDeviceProcAddr },
#define ADD_HOOK(fn) { "vk" # fn, (void *) overlay_ ## fn }
#define ADD_ALIAS_HOOK(alias, fn) { "vk" # alias, (void *) overlay_ ## fn }
   ADD_HOOK(AllocateCommandBuffers),
   ADD_HOOK(FreeCommandBuffers),
   ADD_HOOK(ResetCommandBuffer),
   ADD_HOOK(BeginCommandBuffer),
   ADD_HOOK(EndCommandBuffer),
   ADD_HOOK(CmdExecuteCommands),

   ADD_HOOK(CreateSwapchainKHR),
   ADD_HOOK(QueuePresentKHR),
   ADD_HOOK(DestroySwapchainKHR),

   ADD_HOOK(QueueSubmit),

   ADD_HOOK(CreateDevice),
   ADD_HOOK(DestroyDevice),

   ADD_HOOK(CreateInstance),
   ADD_HOOK(DestroyInstance),
#undef ADD_HOOK
};

static void *find_ptr(const char *name)
{
    std::string f(name);

    if (is_blacklisted() && (f != "vkCreateInstance" && f != "vkDestroyInstance" && f != "vkCreateDevice" && f != "vkDestroyDevice"))
    {
        return NULL;
    }

   for (uint32_t i = 0; i < ARRAY_SIZE(name_to_funcptr_map); i++) {
      if (strcmp(name, name_to_funcptr_map[i].name) == 0)
         return name_to_funcptr_map[i].ptr;
   }

   return NULL;
}

extern "C" VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL overlay_GetDeviceProcAddr(VkDevice dev,
                                                                             const char *funcName)
{
   void *ptr = find_ptr(funcName);
   if (ptr) return reinterpret_cast<PFN_vkVoidFunction>(ptr);

   if (dev == NULL) return NULL;

   struct device_data *device_data = FIND(struct device_data, dev);
   if (device_data->vtable.GetDeviceProcAddr == NULL) return NULL;
   return device_data->vtable.GetDeviceProcAddr(dev, funcName);
}

extern "C" VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL overlay_GetInstanceProcAddr(VkInstance instance,
                                                                               const char *funcName)
{
   void *ptr = find_ptr(funcName);
   if (ptr) return reinterpret_cast<PFN_vkVoidFunction>(ptr);

   if (instance == NULL) return NULL;

   struct instance_data *instance_data = FIND(struct instance_data, instance);
   if (instance_data->vtable.GetInstanceProcAddr == NULL) return NULL;
   return instance_data->vtable.GetInstanceProcAddr(instance, funcName);
}
