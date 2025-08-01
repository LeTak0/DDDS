#include <stdlib.h>
#include <3ds.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <3ds/applets/swkbd.h>

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

static void print_status(void);
static bool edit_single_entry(DynEntry *e);
static void save_config(void);
static void edit_entries(void);
static bool kbInput(const char *hint, const char *initial,
                    char *buf, size_t len);

static size_t base64_encode(const uint8_t *src, size_t len,
                            char *out, size_t outsz);

typedef struct {
    const char *name;
    const char *urlFmt;
    bool useBasicAuth;
} ProvInfo;

// For Basic‑Auth providers put Base64("user:pass") in the secret column
static const ProvInfo provTable[] = {
    { "duckdns",
      "https://www.duckdns.org/update?domains=%s&token=%s&ip=", false },
    { "noip",
      "https://dynupdate.no-ip.com/nic/update?hostname=%s&myip=", true },
};

static const ProvInfo* findProvider(const char *name) {
    for (size_t i = 0; i < sizeof(provTable)/sizeof(provTable[0]); ++i)
        if (strcmp(name, provTable[i].name) == 0) return &provTable[i];
    return NULL;
}
// Run the update for each entry
// If the provider is unknown, log it to error.log
static size_t base64_encode(const uint8_t *src, size_t len,
                            char *out, size_t outsz)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t chunk = src[i] << 16;
        if (i + 1 < len) chunk |= src[i + 1] << 8;
        if (i + 2 < len) chunk |= src[i + 2];
        for (int j = 18; j >= 0 && out_len < outsz - 1; j -= 6) {
            out[out_len++] = tbl[(chunk >> j) & 0x3F];
        }
    }
    out[out_len] = '\0';
    return out_len;
}

void run_entries(void) {
    FILE *elog = fopen("ddds.log", "a");
    for (int i = 0; i < entry_count; i++) {
        const ProvInfo *pi = findProvider(entries[i].provider);
        if (!pi) {
            if (elog) fprintf(elog, "Unknown provider: %s\n", entries[i].provider);
            continue;
        }

        /* If provider needs Basic-Auth but the secret field is empty,
           compose it from user:pass and Base64-encode it */
        char autoSecret[128] = {0};
        if (pi->useBasicAuth && !entries[i].secret[0] &&
            entries[i].user[0] && entries[i].pass[0]) {
            char concat[128];
            snprintf(concat, sizeof(concat), "%s:%s",
                     entries[i].user, entries[i].pass);
            base64_encode((const uint8_t*)concat, strlen(concat),
                          autoSecret, sizeof(autoSecret));
            strncpy(entries[i].secret, autoSecret, sizeof(entries[i].secret)-1);
        }

        char url[512];
        // Build request URL//
        if (pi->useBasicAuth)
            snprintf(url, sizeof(url), pi->urlFmt, entries[i].domain);
        else
            snprintf(url, sizeof(url), pi->urlFmt,
                     entries[i].domain, entries[i].secret);
        //printf("[DDDS] final URL: %s\n", url); //DEBUG
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
        httpcAddRequestHeaderField(&ctx, "User-Agent",
                                   "DDDS-3DS/1.0 (github.com/yourname/ddds)"); // Custom User-Agent lol. Don't ask why.
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
            gspWaitForVBlank();   // sync to display, avoid glitches
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

// Simple helper that shows the software keyboard
static bool kbInput(const char *hint, const char *initial,
                    char *buf, size_t len)
{
    SwkbdState kbd;
    swkbdInit(&kbd, SWKBD_TYPE_NORMAL, 1, len - 1);
    swkbdSetHintText(&kbd, hint);
    swkbdSetInitialText(&kbd, initial ? initial : "");
    swkbdSetButton(&kbd, SWKBD_BUTTON_LEFT, "Cancel", false);
    SwkbdButton btn = swkbdInputText(&kbd, buf, len);
    // Any button other than SWKBD_BUTTON_RIGHT is treated as cancel
    return (btn == SWKBD_BUTTON_RIGHT);   // OK/Confirm
}

// Interactive field‑by‑field editor for a single DynEntry.
// Move with Up/Down, press A to edit the highlighted field,
// and exit with X.
// Redraw the single‑entry editor screen
static void draw_entry_editor(const DynEntry *e, int sel)
{
    consoleClear();
    printf("Edit DynDNS Entry  (X = back)\n");
    printf("--------------------------------\n");
    printf(" %c Provider : %s\n", sel == 0 ? '#' : ' ', e->provider);
    printf(" %c Domain   : %s\n", sel == 1 ? '#' : ' ', e->domain);
    printf(" %c Secret   : %s\n", sel == 2 ? '#' : ' ', e->secret);
    printf(" %c Username : %s\n", sel == 3 ? '#' : ' ', e->user);
    printf(" %c Password : %s\n", sel == 4 ? '#' : ' ', e->pass);
    printf("\nUp/Down Move   A = Edit   X = Done\n");
}

// Edit a single DynEntry, return true if it was changed
static bool edit_single_entry(DynEntry *e)
{
    int sel = 0;                // 0=provider 1=domain 2=secret 3=user 4=pass
    char buf[128] = {0};
    bool changed = false;
    bool redraw = true;         // initial paint

    while (aptMainLoop())
    {
        if (redraw) {
            draw_entry_editor(e, sel);
            redraw = false;
        }

        gspWaitForVBlank();
        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown & KEY_X)
            break;

        if (kDown & KEY_UP) {
            sel = (sel == 0) ? 4 : sel - 1;
            redraw = true;
        } else if (kDown & KEY_DOWN) {
            sel = (sel + 1) % 5;
            redraw = true;
        } else if (kDown & KEY_A) {
            memset(buf, 0, sizeof(buf));
            switch (sel) {
                case 0:
                    if (kbInput("Provider (duckdns/noip)", e->provider,
                                buf, sizeof(buf))) {
                        strncpy(e->provider, buf, sizeof(e->provider) - 1);
                        changed = true;
                    }
                    break;
                case 1:
                    if (kbInput("Domain", e->domain, buf, sizeof(buf))) {
                        strncpy(e->domain, buf, sizeof(e->domain) - 1);
                        changed = true;
                    }
                    break;
                case 2:
                    if (kbInput("Secret / Token (opt)", e->secret,
                                buf, sizeof(buf))) {
                        strncpy(e->secret, buf, sizeof(e->secret) - 1);
                        changed = true;
                    }
                    break;
                case 3:
                    if (kbInput("Username (opt)", e->user,
                                buf, sizeof(buf))) {
                        strncpy(e->user, buf, sizeof(e->user) - 1);
                        changed = true;
                    }
                    break;
                case 4:
                    if (kbInput("Password (opt)", e->pass,
                                buf, sizeof(buf))) {
                        strncpy(e->pass, buf, sizeof(e->pass) - 1);
                        changed = true;
                    }
                    break;
            }
            redraw = true;   // re‑paint after editing
        }
    }
    return changed;
}

// Write the current config back to disk
static void save_config(void)
{
    FILE *f = fopen(CONFIG_FILENAME, "w");
    if (!f) return;
    fprintf(f, "interval,%d\n", update_interval_sec);
    for (int i = 0; i < entry_count; ++i)
        fprintf(f, "%s,%s,%s,%s,%s\n",
                entries[i].provider,
                entries[i].domain,
                entries[i].secret,
                entries[i].user,
                entries[i].pass);
    fclose(f);
}

// Draw the editor screen for the given selection
static void draw_editor(int sel)
{
    consoleClear();
    printf("DDDS Config - press X to return\n");
    printf("--------------------------------\n");

    /* Entry list ---------------------------------------------------------- */
    for (int i = 0; i < entry_count; ++i)
        printf(" %c [%d] %s,%s\n", sel == i ? '#' : ' ', i,
               entries[i].provider, entries[i].domain);

    /* Optional <Add new> slot -------------------------------------------- */
    int addIdx = -1;
    if (entry_count < MAX_ENTRIES) {
        addIdx = entry_count;
        printf(" %c [%d] <Add new>\n", sel == addIdx ? '#' : ' ', addIdx);
    }

    /* Interval editor ----------------------------------------------------- */
    int intervalIdx = (entry_count < MAX_ENTRIES) ? entry_count + 1
                                                  : entry_count;
    printf(" %c [I] Interval : %d s\n", sel == intervalIdx ? '#' : ' ',
           update_interval_sec);

    printf("\nUp/Down = Move   A = Select   X = Back\n");
}

// Re‑print the main status screen
static void print_status(void)
{
    consoleClear();
    printf("DDDS - Dynamic DNS Updater\n");
    printf("--------------------------------\n");
    printf("Press X to edit entries.\n");
    printf("Press START to exit.\n");
    printf("Parsed %d DynDNS entries\n", entry_count);
    for (int i = 0; i < entry_count; ++i)
        printf(" [%d] %s,%s\n", i, entries[i].provider, entries[i].domain);
    printf("Polling every %d seconds\n", update_interval_sec);
}

// Interactive editor, opened with KEY_X
static void edit_entries(void)
{
    int sel = 0;
    draw_editor(sel);
    while (aptMainLoop())
    {
        gspWaitForVBlank();
        hidScanInput();

        int addIdx = (entry_count < MAX_ENTRIES) ? entry_count : -1;
        int intervalIdx = (entry_count < MAX_ENTRIES) ? entry_count + 1
                                                      : entry_count;
        int lastIdx = intervalIdx;

        if (hidKeysDown() & KEY_X) break;

        if (hidKeysDown() & KEY_UP)
        {
            sel = (sel == 0) ? lastIdx : sel - 1;
            draw_editor(sel);
        }
        if (hidKeysDown() & KEY_DOWN)
        {
            sel = (sel == lastIdx) ? 0 : sel + 1;
            draw_editor(sel);
        }

        if (hidKeysDown() & KEY_A)
        {
            if (sel == addIdx)        /* Add new */
            {
                if (entry_count >= MAX_ENTRIES) continue;
                DynEntry *e = &entries[entry_count];
                memset(e, 0, sizeof(DynEntry));
                if (edit_single_entry(e) && e->provider[0] && e->domain[0]) {
                    entry_count++;
                }
            }
            else if (sel == intervalIdx)  /* Edit polling interval */
            {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", update_interval_sec);
                if (kbInput("Polling interval (sec)", buf, buf, sizeof(buf))) {
                    int v = atoi(buf);
                    if (v > 0) {
                        update_interval_sec = v;
                        save_config();
                    }
                }
            }
            else                            /* Edit or delete existing */
            {
                consoleClear();
                printf("A = Edit   Y = Delete   B = Back\n");
                while (aptMainLoop())
                {
                    hidScanInput();
                    u32 kd = hidKeysDown();
                    if (kd & KEY_B) break;
                    if (kd & KEY_A)          /* Edit */
                    {
                        edit_single_entry(&entries[sel]);
                        break;
                    }
                    if (kd & KEY_Y)          /* Delete */
                    {
                        for (int j = sel; j < entry_count - 1; ++j)
                            entries[j] = entries[j + 1];
                        entry_count--;
                        if (sel >= entry_count) sel = entry_count - 1;
                        break;
                    }
                    gspWaitForVBlank();
                }
                save_config();
            }
            draw_editor(sel);
        }
    }

    print_status();   // restore main screen when exiting editor
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
    print_status();



    Result rc = httpcInit(0x100000); // Initialize HTTP client with 1MB buffer
    if (R_FAILED(rc)) {
        printf("httpcInit failed – exiting.\n");
        waitForA();
        goto exit_app;
    }

    // Keep the system awake //
    aptSetSleepAllowed(false);

    while (aptMainLoop()) {
        gspWaitForVBlank();
        hidScanInput();

        if (hidKeysDown() & KEY_X) {      // open configuration editor
            edit_entries();
        }
        if (hidKeysDown() & KEY_START) break;  // Exit

        run_entries();

        // Sleep in 100ms slices so START and HOME remain responsive //
        const s64 slice = 100000000LL;   // 100ms
        s64 slept = 0;
        while (slept < (s64)update_interval_sec * 1000000000LL) {
            svcSleepThread(slice);
            slept += slice;

            if (!aptMainLoop()) goto exit_app;      // HOME pressed or system event
            hidScanInput();
            if (hidKeysDown() & KEY_X) {      // open configuration editor
                edit_entries();
            }
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