#include <deque>
#include <list>
#include <map>
#include <vector>

#include <sicm.hpp>

int main() {
    sicm_device_list devs = sicm_init();

    // the actual check is not crashing
    for(unsigned int i = 0; i < devs.count; i += 3) {
        SICMDeviceAllocator <int> sa(devs.devices[i]);

        // deque
        {
            std::size_t size = 1;
            for(std::size_t i = 0; i < 5; i++) {
                std::deque <int, SICMDeviceAllocator <int> > deque(sa);
                deque.resize(size);
                size *= 10;
            }
        }

        // list
        {
            std::size_t size = 1;
            for(std::size_t i = 0; i < 5; i++) {
                std::list <int, SICMDeviceAllocator <int> > list(sa);
                list.resize(size);
                size *= 10;
            }
        }

        // map
        {
            std::map <int, int, std::less <int>, SICMDeviceAllocator <std::pair<const int, int> > > map(sa);
            for(std::size_t i = 0; i < 5; i++) {
                map[i] = i;
            }
        }

        // vector
        {
            std::size_t size = 1;
            for(std::size_t i = 0; i < 5; i++) {
                std::vector <int, SICMDeviceAllocator <int> > vector(sa);
                vector.resize(size);
                size *= 10;
            }
        }
    }

    sicm_fini();
    return 0;
}
