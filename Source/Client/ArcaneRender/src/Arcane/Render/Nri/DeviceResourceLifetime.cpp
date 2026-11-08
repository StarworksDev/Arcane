#include "DeviceResourceLifetime.hpp"

namespace Arcane
{
    void DeviceResourceLifetime::NoteSample(std::uint64_t slotKey, const void* context, std::uint64_t fence)
    {
        std::vector<LastUse>& uses = m_uses[slotKey];
        for (LastUse& u : uses)
        {
            if (u.context == context)
            {
                if (fence > u.fence)
                    u.fence = fence;
                return;
            }
        }
        uses.push_back(LastUse{ context, fence });
    }

    bool DeviceResourceLifetime::AllRetired(std::uint64_t slotKey, CompletedFn completed, void* user) const
    {
        const auto it = m_uses.find(slotKey);
        if (it == m_uses.end())
            return true;
        for (const LastUse& u : it->second)
        {
            // Without a completion query nothing can be proven retired.
            if (!completed || !completed(u.context, u.fence, user))
                return false;
        }
        return true;
    }

    void DeviceResourceLifetime::Clear(std::uint64_t slotKey)
    {
        m_uses.erase(slotKey);
    }

    void DeviceResourceLifetime::ForgetContext(const void* context)
    {
        for (auto it = m_uses.begin(); it != m_uses.end();)
        {
            std::vector<LastUse>& uses = it->second;
            for (std::size_t i = 0; i < uses.size();)
            {
                if (uses[i].context == context) { uses[i] = uses.back(); uses.pop_back(); }
                else ++i;
            }
            if (uses.empty()) it = m_uses.erase(it);
            else ++it;
        }
    }

    const std::vector<LastUse>* DeviceResourceLifetime::Uses(std::uint64_t slotKey) const
    {
        const auto it = m_uses.find(slotKey);
        return it == m_uses.end() ? nullptr : &it->second;
    }
}
