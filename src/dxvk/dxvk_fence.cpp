#include "dxvk_device.h"
#include "dxvk_fence.h"

namespace dxvk {

  DxvkFence::DxvkFence(
          DxvkDevice*           device,
    const DxvkFenceCreateInfo&  info)
  : m_vkd(device->vkd()), m_info(info) {
    if (!device->features().khrTimelineSemaphore.timelineSemaphore)
      throw DxvkError("Timeline semaphores not supported");

    VkSemaphoreTypeCreateInfoKHR typeInfo;
    typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR;
    typeInfo.pNext = nullptr;
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;
    typeInfo.initialValue = info.initialValue;
    
    VkSemaphoreCreateInfo semaphoreInfo;
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphoreInfo.pNext = &typeInfo;
    semaphoreInfo.flags = 0;

    VkResult vr = m_vkd->vkCreateSemaphore(m_vkd->device(),
      &semaphoreInfo, nullptr, &m_semaphore);

    if (vr != VK_SUCCESS)
      throw DxvkError("Failed to create timeline semaphore");

    m_thread = dxvk::thread([this] { run(); });
  }


  DxvkFence::~DxvkFence() {
    m_stop.store(true);
    m_thread.join();

    m_vkd->vkDestroySemaphore(m_vkd->device(), m_semaphore, nullptr);
  }


  void DxvkFence::enqueueWait(uint64_t value, DxvkFenceEvent&& event) {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    if (value > m_lastValue.load())
      m_queue.emplace(value, std::move(event));
    else
      event();
  }
  

  void DxvkFence::run() {
    uint64_t value = 0ull;

    VkSemaphoreWaitInfoKHR waitInfo;
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR;
    waitInfo.pNext = nullptr;
    waitInfo.flags = 0;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &m_semaphore;
    waitInfo.pValues = &value;

    while (!m_stop.load()) {
      std::unique_lock<dxvk::mutex> lock(m_mutex);

      // Query actual semaphore value and start from there, so that
      // we can skip over large increments in the semaphore value
      VkResult vr = m_vkd->vkGetSemaphoreCounterValueKHR(m_vkd->device(), m_semaphore, &value);

      if (vr != VK_SUCCESS) {
        Logger::err(str::format("Failed to query semaphore value: ", vr));
        return;
      }

      m_lastValue.store(value);

      // Signal all enqueued events whose value is not greater than
      // the current semaphore value
      while (!m_queue.empty() && m_queue.top().value <= value) {
        m_queue.top().event();
        m_queue.pop();
      }

      if (m_stop)
        return;

      lock.unlock();

      // Wait for the semaphore to be singaled again and update
      // state. The timeout is chosen so that we can free the
      // fence object without having to 
      value += 1;

      vr = m_vkd->vkWaitSemaphoresKHR(
        m_vkd->device(), &waitInfo, 10'000'000ull);

      if (vr != VK_SUCCESS && vr != VK_TIMEOUT) {
        Logger::err(str::format("Failed to wait for semaphore: ", vr));
        return;
      }
    }
  }


  DxvkFenceList::DxvkFenceList() {

  }


  DxvkFenceList::~DxvkFenceList() {

  }


  void DxvkFenceList::add(Rc<DxvkFence> fence, uint64_t value) {
    m_fences.emplace_back(std::move(fence), value);
  }


  void DxvkFenceList::reset() {
    m_fences.clear();
  }

}
