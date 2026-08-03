#include <soc/soc.h>

int main(void)
{
    return soc_get_abi_version() == SOC_ABI_VERSION ? 0 : 1;
}
