#include <stdlib.h>
#include <3ds.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int update_interval_sec = 300;   // default 5min
#define CONFIG_FILENAME "config.txt"

//Dynamic DNS Entry structure
typedef struct {
    char provider[32];
    char domain[64];
    char secret[128];
    char user[64];
    char pass[64];
} DynEntry;


#define MAX_ENTRIES 10
DynEntry entries[MAX_ENTRIES];
int entry_count = 0;

typedef enum { PROV_DUCKDNS, PROV_NOIP, PROV_UNKNOWN } ProvType;

typedef struct {
    const char *name;
    ProvType type;
    const char *urlFmt;
    bool useBasicAuth;
} ProvInfo;

// For Basic‑Auth providers put Base64("user:pass") in the secret column
static const ProvInfo provTable[] = {
    { "duckdns", PROV_DUCKDNS,
      "https://www.duckdns.org/update?domains=%s&token=%s&ip=", false },
    { "noip",    PROV_NOIP,
      "https://dynupdate.no-ip.com/nic/update?hostname=%s&myip=", true  },
};

static const ProvInfo* findProvider(const char *name) {
    for (size_t i = 0; i < sizeof(provTable)/sizeof(provTable[0]); ++i)
        if (strcmp(name, provTable[i].name) == 0) return &provTable[i];
    return NULL;
}
// Run the update for each entry
// If the provider is unknown, log it to error.log
void run_entries(void) {
    FILE *elog = fopen("ddds.log", "a");
    for (int i = 0; i < entry_count; i++) {
        const ProvInfo *pi = findProvider(entries[i].provider);
        if (!pi) {
            if (elog) fprintf(elog, "Unknown provider: %s\n", entries[i].provider);
            continue;
        }

        char url[512];
        snprintf(url, sizeof(url), pi->urlFmt, entries[i].domain, entries[i].secret);
        // If the provider requires Basic Auth, the secret should contain Base64("user:pass")
        httpcContext ctx;
        Result rc = httpcOpenContext(&ctx, HTTPC_METHOD_GET, url, 0);
        if (R_FAILED(rc)) {
            if (elog) fprintf(elog, "%s httpcOpen err 0x%08lX\n", entries[i].domain, rc);
            continue;
        }
        // Set request headers
        if (pi->useBasicAuth && entries[i].secret[0]) {
            char header[192];
            snprintf(header, sizeof(header), "Basic %s", entries[i].secret);
            httpcAddRequestHeaderField(&ctx, "Authorization", header);
        }
        httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
        httpcSetKeepAlive(&ctx, 1);
        rc = httpcBeginRequest(&ctx);
        if (R_SUCCEEDED(rc)) {
            u32 status;
            httpcGetResponseStatusCode(&ctx, &status);
            // format current UTC time YYYY‑MM‑DD HH:MM:SS //
            time_t now = time(NULL);
            struct tm tm_now;
            gmtime_r(&now, &tm_now);
            char ts[20];
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
            printf("%s - Updated %s (status %lu)\n", ts, entries[i].domain, status);
            if (status < 200 || status >= 300) {
                if (elog) fprintf(elog, "%s HTTP %lu\n", entries[i].domain, status);
            }
        } else {
            if (elog) fprintf(elog, "%s beginReq err 0x%08lX\n", entries[i].domain, rc);
        }
        httpcCloseContext(&ctx);
    }
    if (elog) fclose(elog);
}
// Wait for the user to press A, pause the app until then
static void waitForA(void) {
    printf("Press A to continue...\n");
    while (true) {
        gspWaitForVBlank();
        hidScanInput();
        if (hidKeysDown() & KEY_A) break;
    }
}

int main(int argc, char** argv) {
    gfxInitDefault(); // Initialize graphics
    consoleInit(GFX_TOP, NULL); // Initialize console on the top screen
    fsInit(); // Initialize file system
    static u32 soc_sharedmem[0x100000 / sizeof(u32)]; // 1MB shared memory for SOC
    socInit(soc_sharedmem, sizeof(soc_sharedmem)); // Initialize SOC
    // Keep Wi‑Fi active while the lid is closed
    ndmuInit(); // Initialize NDMU (Network Device Management Unit)
    NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE); // Enter exclusive state for Wi‑Fi
    FILE* file = fopen(CONFIG_FILENAME, "r");   // open relative to current dir
    if (!file) {
        FILE *elog = fopen("ddds.log", "a");
        if (elog) {
            time_t now = time(NULL);
            struct tm tm_now;
            gmtime_r(&now, &tm_now);
            char ts[20];
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
            fprintf(elog, "%s - Could not find %s\n", ts, CONFIG_FILENAME);
            fclose(elog);
        }
        printf("Could not find %s.\n", CONFIG_FILENAME);
        printf("Place it next to DDDS.3dsx.\n");
        waitForA();
        gfxExit();
        return 1;
    }

    char line[256];
    while (fgets(line, sizeof(line), file) && entry_count < MAX_ENTRIES) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue; // skip comments/blank
        char *tok = strtok(line, ",\r\n");

        /* Special directive: interval,<seconds> */
        if (strncmp(tok, "interval", 8) == 0) {
            tok = strtok(NULL, ",\r\n");
            if (tok) update_interval_sec = atoi(tok);
            continue;
        }

        DynEntry *e = &entries[entry_count];
        memset(e, 0, sizeof(DynEntry));
        int col = 0;
        while (tok && col < 5) {
            switch(col) {
                case 0: strncpy(e->provider, tok, sizeof(e->provider)-1); break;
                case 1: strncpy(e->domain,   tok, sizeof(e->domain)-1);   break;
                case 2: strncpy(e->secret,   tok, sizeof(e->secret)-1);   break;
                case 3: strncpy(e->user,     tok, sizeof(e->user)-1);     break;
                case 4: strncpy(e->pass,     tok, sizeof(e->pass)-1);     break;
            }
            tok = strtok(NULL, ",\r\n");
            col++;
        }
        if (col >= 2) entry_count++;   // provider+domain loaded
    }
    fclose(file);
    printf("Press START to exit.\n");
    printf("Parsed %d DynDNS entries\n", entry_count);
    for (int i = 0; i < entry_count; ++i) {
        printf(" [%d] %s,%s\n", i, entries[i].provider, entries[i].domain);
    }
    printf("Polling every %d seconds\n", update_interval_sec);


    Result rc = httpcInit(0x100000); // Initialize HTTP client with 1MB buffer
    if (R_FAILED(rc)) {
        printf("httpcInit failed – exiting.\n");
        waitForA();
        goto exit_app;
    }

    // Keep the system awake //
    aptSetSleepAllowed(false);

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) break;  // Exit

        run_entries();

        // Sleep in 100ms slices so START and HOME remain responsive //
        const s64 slice = 100000000LL;   // 100ms
        s64 slept = 0;
        while (slept < (s64)update_interval_sec * 1000000000LL) {
            svcSleepThread(slice);
            slept += slice;

            if (!aptMainLoop()) goto exit_app;      // HOME pressed or system event
            hidScanInput();
            if (hidKeysDown() & KEY_START) goto exit_app;
        }
    }

exit_app: // Exit the app gracefully
    NDMU_LeaveExclusiveState();
    ndmuExit();
    httpcExit();
    fsExit();
    gfxExit();
    return 0;
}