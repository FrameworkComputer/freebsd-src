#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

// TODO:
// - Make it a hierarchy
//   dev.package.energy_uj
//   dev.pp0.energy_uj
//   dev.pp1.energy_uj
//   dev.psys.energy_uj
// - Detect which domains are present
// - Why is uncore 0? I think on Linux it's the same
//   - Oh I think it's the iGPU
// - Integrate as in-tree module (Can we still compile and load it manually?)

// TODO: How to include it?
// #include <sys/specialreg.h>
#define MSR_RAPL_POWER_UNIT     0x606

#define MSR_PKG_ENERGY_STATUS   0x611
#define MSR_DRAM_ENERGY_STATUS  0x619
#define MSR_PP0_ENERGY_STATUS   0x639
#define MSR_PP1_ENERGY_STATUS   0x641

/* Package RAPL Domain */
#define MSR_PKG_RAPL_POWER_LIMIT        0x610
#define MSR_PKG_ENERGY_STATUS           0x611
#define MSR_PKG_PERF_STATUS             0x613
#define MSR_PKG_POWER_INFO              0x614

/* PP0 RAPL Domain */
#define MSR_PP0_POWER_LIMIT             0x638
#define MSR_PP0_ENERGY_STATUS           0x639
#define MSR_PP0_POLICY                  0x63A
#define MSR_PP0_PERF_STATUS             0x63B

/* PP1 RAPL Domain, may reflect to uncore devices */
#define MSR_PP1_POWER_LIMIT             0x640
#define MSR_PP1_ENERGY_STATUS           0x641
#define MSR_PP1_POLICY                  0x642

/* DRAM RAPL Domain */
#define MSR_DRAM_POWER_LIMIT            0x618
#define MSR_DRAM_ENERGY_STATUS          0x619
#define MSR_DRAM_PERF_STATUS            0x61B
#define MSR_DRAM_POWER_INFO             0x61C

/* PSYS RAPL Domain */
#define MSR_PLATFORM_ENERGY_STATUS      0x64D

/* RAPL UNIT BITMASK */
#define POWER_UNIT_OFFSET       0
#define POWER_UNIT_MASK         0x0F

#define ENERGY_UNIT_OFFSET      0x08
#define ENERGY_UNIT_MASK        0x1F00

#define TIME_UNIT_OFFSET        0x10
#define TIME_UNIT_MASK          0xF000

static void update_energy(void);

static uint64_t power_divisor;
static uint64_t energy_divisor;
static uint64_t time_divisor;

static int
sysctl_handle_u64_range(SYSCTL_HANDLER_ARGS)
{
  uint64_t value;

  update_energy();

  value = *(uint64_t *)arg1;
  return sysctl_handle_64(oidp, &value, 0, req);
}

static uint64_t package_uj;
SYSCTL_PROC(_dev, OID_AUTO, package_uj, CTLFLAG_RD | CTLTYPE_U64,
    &package_uj, 0, sysctl_handle_u64_range, "QU", "Package Energy");
static uint64_t pp0_uj;
SYSCTL_PROC(_dev, OID_AUTO, pp0_uj, CTLFLAG_RD | CTLTYPE_U64,
    &pp0_uj, 0, sysctl_handle_u64_range, "QU", "PP0 Energy");
static uint64_t pp1_uj;
SYSCTL_PROC(_dev, OID_AUTO, pp1_uj, CTLFLAG_RD | CTLTYPE_U64,
    &pp1_uj, 0, sysctl_handle_u64_range, "QU", "PP1 Energy");
static uint64_t psys_uj;
SYSCTL_PROC(_dev, OID_AUTO, psys_uj, CTLFLAG_RD | CTLTYPE_U64,
    &psys_uj, 0, sysctl_handle_u64_range, "QU", "PSYS Energy");

static void update_energy(void) {
  uint64_t package = rdmsr(MSR_PKG_ENERGY_STATUS) & 0xFFFFFFFF;
  uint64_t pp0 = rdmsr(MSR_PP0_ENERGY_STATUS);
  uint64_t pp1 = rdmsr(MSR_PP1_ENERGY_STATUS);
  uint64_t psys = rdmsr(MSR_PLATFORM_ENERGY_STATUS);

  package_uj = package / energy_divisor * 1000000;
  pp0_uj = pp0 / energy_divisor * 1000000;
  pp1_uj = pp1 / energy_divisor * 1000000;
  psys_uj = psys / energy_divisor * 1000000;
}

static void print_energy(void) {
  // Total amouont of energy consumed since last cleared
  uprintf("  Energy Measurements\n");
  uprintf("    package:   %10ld uJ\n", package_uj);
  uprintf("    pp0:       %10ld uJ\n", pp0_uj);
  uprintf("    pp1:       %10ld uJ\n", pp1_uj);
  uprintf("    psys:      %10ld uJ\n", psys_uj);
}

static void init_rapl(void) {
  uint64_t units = rdmsr(MSR_RAPL_POWER_UNIT);
  power_divisor = 1 << (units & 0x0F);
  energy_divisor = 1 << ((units >> 8) & 0x1F);
  time_divisor = 1 << ((units >> 16) & 0x0F);
}

static void rapl_details(void) {
  uprintf("  Power:         1/%ldW\n", power_divisor);
  uprintf("  Energy:        1/%ldJ\n", energy_divisor);
  uprintf("  Time:          1/%lds\n", time_divisor);

  uprintf("Package thermals");
  uint64_t power_info = rdmsr(MSR_PKG_POWER_INFO);
  uprintf("MSR_PKG_POWER_INFO: %08lX\n", power_info);

  int spec = (power_info & 0x7FFF) / power_divisor;
  uprintf("  Thermal Spec:  %2d W\n", spec);

  int min_power = ((power_info >> 16) & 0x7fff) / power_divisor;
  uprintf("  Minimum Power: %2d W\n", min_power);

  int max_power = ((power_info >> 32) & 0x7fff) / power_divisor;
  uprintf("  Maximum Power: %2d W\n", max_power);

  // TODO: Here need to show decimals
  int max_time = ((power_info >> 48) & 0x7fff) / time_divisor;
  uprintf("  Maximum Time:  %2d s\n", max_time);

  uprintf("Package power limits\n");
  // Writes are locked until reset (probably by BIOS)
  uint64_t limits = rdmsr(MSR_PKG_RAPL_POWER_LIMIT);

  bool locked = (limits >> 63) != 0;
  uprintf("  Locked:        %2d\n", locked);

  int pl1_power = (limits & 0x7FFF) / power_divisor;
  int pl1_window = (((limits >> 17) & 0x007F) * 1000) / time_divisor;
  bool pl1_enabled = (limits & (1LL << 15)) != 0;
  bool pl1_clamped = (limits & (1LL << 16)) != 0;
  uprintf("  PL1\n");
  uprintf("    Power:       %2d W\n", pl1_power);
  uprintf("    Time Window: %2d.%d s\n", pl1_window / 1000, pl1_window % 1000);
  uprintf("    Enabled:     %2d\n", pl1_enabled);
  uprintf("    Clamped:     %2d\n", pl1_clamped);

  int pl2_power = ((limits >> 32) & 0x7FFF) / power_divisor;
  int pl2_window = (((limits >> 49) & 0x007F) * 1000) / time_divisor;
  bool pl2_enabled = (limits & (1LL << 47)) != 0;
  bool pl2_clamped = (limits & (1LL << 48)) != 0;
  uprintf("  PL2\n");
  uprintf("    Power:       %2d W\n", pl2_power);
  uprintf("    Time Window: %2d.%d s\n", pl2_window / 1000, pl2_window % 1000);
  uprintf("    Enabled:     %2d\n", pl2_enabled);
  uprintf("    Clamped:     %2d\n", pl2_clamped);

  update_energy();
  print_energy();
}

static int intel_rapl_event_handler(struct module *module, int event_type, void *arg) {

  int retval = 0;

  switch (event_type) {
    case MOD_LOAD:
      uprintf("Intel RAPL Loaded\n");
      init_rapl();
      update_energy();
      rapl_details();
      break;

    case MOD_UNLOAD:
      uprintf("Intel RAPL Unloaded\n");
      break;

    default:
      retval = EOPNOTSUPP;
      break;
  }

  return(retval);
}

static moduledata_t intel_rapl_data = {
  "intel_rapl",
  intel_rapl_event_handler,
  NULL  // TODO
};

DECLARE_MODULE(freebsd_intel_rapl, intel_rapl_data, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
