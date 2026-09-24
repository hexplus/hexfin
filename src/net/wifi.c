/* See net/wifi.h.
 *
 * Only compiled for the console; scripts/test.sh compiles every .c under
 * src/net and none of this exists there. */
#if defined(__PSP__)

#include "net/wifi.h"

#include "net/net_modules.h"
#include "platform/psp_platform.h"

#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <psputility.h>
#include <psputility_netparam.h>

#include <stddef.h>

/* sceNetInit's pool.
 *
 * 128 kB is the figure every PSP network sample has used since the SDK
 * shipped, and it is the one this project starts from rather than a number of
 * its own invention. docs/RESEARCH.md section 6 records that sceNetInit costs
 * roughly 512 kB in total -- this pool is only the part we choose; the
 * threads and the loaded PRXs are the rest. The probe reports the partition
 * before and after, so the real cost is measured rather than argued about.
 *
 * The two threads get the SDK's own defaults: priority 0x20 sits below this
 * program's main thread, and the stack size is documented as being forced to
 * 4096 on anything past 1.5 firmware regardless of what is passed. */
#define WIFI_POOL_SIZE 0x20000
#define WIFI_THREAD_PRIO 0x20
#define WIFI_THREAD_STACK 0x1000

/* The apctl thread's own stack and priority, the values the SDK's examples
 * use. It is the thread that drives the association, and it must be able to
 * run while this one is sitting in the poll loop below, hence a priority
 * above the network threads'. */
#define WIFI_APCTL_STACK 0x1600
#define WIFI_APCTL_PRIO 0x30

/* How often the state is looked at. Fast enough that HOME feels immediate
 * (design section 3.4 wants the gap between HOME and exit short), slow enough
 * that the poll is free. */
#define WIFI_POLL_MS 100

#define WIFI_ERRLEN 192

/* How many profile slots are looked through when the caller asks for "the
 * first one". Older firmware stores ten and later firmware more; a slot that
 * is not there costs one quick utility call, so looking further is free. */
#define WIFI_MAX_PROFILES 24

/* Every stage that must be undone, in the order it was done, so shutdown can
 * walk it backwards and skip whatever never happened. */
static int g_have_net;
static int g_have_inet;
static int g_have_resolver;
static int g_have_apctl;
static int g_connected;

static char g_err[WIFI_ERRLEN];
static char g_ip[20];
static char g_gateway[20];
static char g_dns[20];
static char g_profile[160];

static void err_set(const char *a, const char *b, int code, int with_code) {
    static const char hex[] = "0123456789ABCDEF";
    size_t            n     = 0;
    int               i;

    while (a && a[n] && n + 1 < WIFI_ERRLEN) {
        g_err[n] = a[n];
        n++;
    }
    if (b) {
        size_t k = 0;
        while (b[k] && n + 1 < WIFI_ERRLEN) g_err[n++] = b[k++];
    }
    if (with_code) {
        const char *suffix = " (firmware returned 0x";
        size_t      s      = 0;
        while (suffix[s] && n + 1 < WIFI_ERRLEN) g_err[n++] = suffix[s++];
        for (i = 28; i >= 0 && n + 1 < WIFI_ERRLEN; i -= 4) g_err[n++] = hex[((unsigned)code >> i) & 0xFu];
        if (n + 1 < WIFI_ERRLEN) g_err[n++] = ')';
    }
    g_err[n] = 0;
}

/* The state a stalled association gave up in is the single most useful thing
 * to put on the screen: "scanning" means the AP was never seen, "joining"
 * means it was seen and the key is wrong, and "getting an address" means the
 * association worked and DHCP did not. Three different things to go and fix. */
static const char *state_name(int state) {
    switch (state) {
        case PSP_NET_APCTL_STATE_DISCONNECTED: return "still disconnected";
        case PSP_NET_APCTL_STATE_SCANNING: return "still scanning for the access point";
        case PSP_NET_APCTL_STATE_JOINING: return "still trying to join the access point";
        case PSP_NET_APCTL_STATE_GETTING_IP: return "joined, but still waiting for an address";
        case PSP_NET_APCTL_STATE_GOT_IP: return "connected";
        case PSP_NET_APCTL_STATE_EAP_AUTH: return "still in EAP authentication";
        case PSP_NET_APCTL_STATE_KEY_EXCHANGE: return "still exchanging keys";
        default: return "in a state the firmware did not name";
    }
}

static void str_append(char *dst, size_t cap, size_t *n, const char *src) {
    while (src && *src && *n + 1 < cap) dst[(*n)++] = *src++;
    dst[*n] = 0;
}

/* "2 Home (MyNetwork)": the slot, the name the PSP's settings screen shows,
 * and the SSID it will actually look for. Put on the screen so that a failure
 * says which profile was used, instead of leaving you to guess. */
static void describe_profile(int index) {
    netData data;
    size_t  n = 0;
    char    num[4];

    g_profile[0] = 0;
    num[0]       = (char)('0' + (index / 10) % 10);
    num[1]       = (char)('0' + index % 10);
    num[2]       = 0;
    str_append(g_profile, sizeof g_profile, &n, index >= 10 ? num : num + 1);

    if (sceUtilityGetNetParam(index, PSP_NETPARAM_NAME, &data) >= 0) {
        data.asString[sizeof data.asString - 1] = 0;
        str_append(g_profile, sizeof g_profile, &n, " ");
        str_append(g_profile, sizeof g_profile, &n, data.asString);
    }
    if (sceUtilityGetNetParam(index, PSP_NETPARAM_SSID, &data) >= 0) {
        data.asString[sizeof data.asString - 1] = 0;
        str_append(g_profile, sizeof g_profile, &n, " (");
        str_append(g_profile, sizeof g_profile, &n, data.asString);
        str_append(g_profile, sizeof g_profile, &n, ")");
    }
}

/* The first slot that holds a profile, or 0 if none does. Slots are not
 * packed: deleting a profile in the PSP's settings can leave slot 1 empty
 * while slot 2 is the only one there, and a hard-coded 1 then fails with
 * 0x80110601 (PSP_NETPARAM_ERROR_BAD_NETCONF). */
static int first_profile(void) {
    int i;
    for (i = 1; i <= WIFI_MAX_PROFILES; i++) {
        if (sceUtilityCheckNetParam(i) == 0) return i;
    }
    return 0;
}

/* One of the dotted-quad strings sceNetApctlGetInfo hands back, copied out
 * of the union; "" when the firmware would not say. */
static void get_info_string(int code, char *out, size_t cap) {
    union SceNetApctlInfo info;
    size_t                i = 0;

    if (sceNetApctlGetInfo(code, &info) >= 0) {
        info.ip[sizeof info.ip - 1] = 0;
        while (info.ip[i] && i + 1 < cap) {
            out[i] = info.ip[i];
            i++;
        }
    }
    out[i] = 0;
}

int wifi_connect(int config_index, unsigned timeout_ms) {
    int      rc;
    unsigned waited = 0;

    g_err[0] = 0;
    g_ip[0]      = 0;
    g_gateway[0] = 0;
    g_dns[0]     = 0;
    g_profile[0] = 0;

    /* Checked before a single module is loaded: an empty slot is a mistake
     * in the settings, not in the radio, and should say so without first
     * paying for a network stack. */
    if (config_index <= 0) {
        config_index = first_profile();
        if (config_index == 0) {
            err_set("no connection is stored; set one up in the PSP's Settings > Network Settings", NULL, 0, 0);
            return -1;
        }
    } else if (sceUtilityCheckNetParam(config_index) != 0) {
        int first = first_profile();
        err_set(first ? "that connection slot is empty; the first stored connection is slot "
                      : "no connection is stored; set one up in the PSP's Settings > Network Settings",
                NULL, 0, 0);
        if (first) {
            describe_profile(first);
            err_set(g_err, g_profile, 0, 0);
            g_profile[0] = 0;
        }
        return -1;
    }
    describe_profile(config_index);

    /* COMMON and INET are what infrastructure Wi-Fi needs, and nothing more
     * is loaded: PARSEURI, PARSEHTTP and HTTP exist only for sceHttp, which
     * docs/RESEARCH.md section 9 explains this project does not use. Three
     * PRXs not loaded is three PRXs not occupying a 24 MB partition. */
    rc = net_module_acquire(PSP_NET_MODULE_COMMON);
    if (rc < 0) {
        err_set("the common network module would not load", NULL, rc, 1);
        return -1;
    }
    rc = net_module_acquire(PSP_NET_MODULE_INET);
    if (rc < 0) {
        err_set("the internet network module would not load", NULL, rc, 1);
        return -1;
    }

    rc = sceNetInit(WIFI_POOL_SIZE, WIFI_THREAD_PRIO, WIFI_THREAD_STACK, WIFI_THREAD_PRIO, WIFI_THREAD_STACK);
    if (rc < 0) {
        err_set("the network stack would not start", NULL, rc, 1);
        return -1;
    }
    g_have_net = 1;

    rc = sceNetInetInit();
    if (rc < 0) {
        err_set("the socket layer would not start", NULL, rc, 1);
        return -1;
    }
    g_have_inet = 1;

    /* The resolver is started here rather than lazily in net/http.c, because
     * starting a firmware subsystem from inside a read path is how a failure
     * arrives halfway through a film instead of before it. */
    rc = sceNetResolverInit();
    if (rc < 0) {
        err_set("the name resolver would not start", NULL, rc, 1);
        return -1;
    }
    g_have_resolver = 1;

    rc = sceNetApctlInit(WIFI_APCTL_STACK, WIFI_APCTL_PRIO);
    if (rc < 0) {
        err_set("the access point controller would not start", NULL, rc, 1);
        return -1;
    }
    g_have_apctl = 1;

    rc = sceNetApctlConnect(config_index);
    if (rc < 0) {
        err_set("the stored access point configuration could not be used", NULL, rc, 1);
        return -1;
    }
    /* Marked connected from HERE, not from success: the association is now
     * under way, and a teardown that skipped the disconnect because the wait
     * below timed out would leave the radio associating into a program that
     * has exited. */
    g_connected = 1;

    for (;;) {
        int state = -1;

        rc = sceNetApctlGetState(&state);
        if (rc < 0) {
            err_set("the access point controller stopped reporting its state", NULL, rc, 1);
            return -1;
        }
        if (state == PSP_NET_APCTL_STATE_GOT_IP) break;

        if (platform_exit_requested()) {
            err_set("the connection was stopped: ", state_name(state), 0, 0);
            return -1;
        }
        if (waited >= timeout_ms) {
            err_set("the access point did not answer in time -- ", state_name(state), 0, 0);
            return -1;
        }

        sceKernelDelayThread(WIFI_POLL_MS * 1000);
        waited += WIFI_POLL_MS;
    }

    /* The address, and the two things a name lookup and a connect depend on.
     * The gateway and DNS server are what DHCP -- or the profile's manual
     * settings -- handed out, and a lookup that fails is usually one of them
     * being wrong, so they go on the screen next to the error. */
    get_info_string(PSP_NET_APCTL_INFO_IP, g_ip, sizeof g_ip);
    get_info_string(PSP_NET_APCTL_INFO_GATEWAY, g_gateway, sizeof g_gateway);
    get_info_string(PSP_NET_APCTL_INFO_PRIMDNS, g_dns, sizeof g_dns);

    /* An association with no address is the failure that reads as success on
     * a screen, so it is refused here rather than left for a socket call to
     * discover. */
    if (g_ip[0] == 0) {
        err_set("the access point reported a connection but gave no address", NULL, 0, 0);
        return -1;
    }

    return 0;
}

const char *wifi_ip(void) { return g_ip; }

const char *wifi_profile(void) { return g_profile; }

const char *wifi_gateway(void) { return g_gateway; }

const char *wifi_dns(void) { return g_dns; }

const char *wifi_error(void) { return g_err; }

void wifi_shutdown(void) {
    /* Backwards, and each step guarded by whether it ever happened. Design
     * section 3.4: the teardown runs after failures too, so "never started"
     * has to be as safe to tear down as "running". */
    if (g_connected) {
        sceNetApctlDisconnect();
        g_connected = 0;
    }
    if (g_have_apctl) {
        sceNetApctlTerm();
        g_have_apctl = 0;
    }
    if (g_have_resolver) {
        sceNetResolverTerm();
        g_have_resolver = 0;
    }
    if (g_have_inet) {
        sceNetInetTerm();
        g_have_inet = 0;
    }
    if (g_have_net) {
        sceNetTerm();
        g_have_net = 0;
    }
    g_ip[0]      = 0;
    g_gateway[0] = 0;
    g_dns[0]     = 0;
}

#endif /* __PSP__ */
