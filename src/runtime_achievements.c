/* Recomp Achievements (third_party/rl_achievements): the trophies the game unlocks go on to
 * RecompLauncher, which shows the unlock, keeps it on the player's profile and counts Beaten and
 * Mastered. The game's 50 trophies are its native achievement list (ids = trophy ids, PSN order);
 * the ones trophies.dat already had are sent as earlier unlocks. Started without RecompLauncher
 * (no RL_SESSION in the environment), every call here returns at once. */
#include "runtime.h"
#include "rl_achievements.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int wanted = -1; /* RL_SESSION set: -1 not looked yet */
static int connected;

static int session(void) {
    if (wanted < 0) {
        const char *value = getenv("RL_SESSION");
        wanted = value && value[0];
    }
    return wanted;
}

/* A PS4 grade as the "gamerscore" the library converts to points: Bronze 5, Silver 10, Gold 25 and
 * Platinum 50 points. */
static uint32_t grade_score(const char *grade) {
    if (!grade) return 10;
    if (!strcmp(grade, "Platinum")) return 100;
    if (!strcmp(grade, "Gold")) return 75;
    if (!strcmp(grade, "Silver")) return 30;
    return 10;
}

void runtime_achievements_define(int id, const char *name, const char *detail, const char *grade, int earlier) {
    if (!session() || id < 0) return;
    pthread_mutex_lock(&lock);
    rl_ach_native_define((uint32_t)id, name, detail && detail[0] ? detail : NULL, grade_score(grade));
    if (earlier) rl_ach_native_earlier((uint32_t)id);
    pthread_mutex_unlock(&lock);
}

/* The launcher's messages (and the list, if it is sent late) are read about 60 times a second. */
static void *pump(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&lock);
        rl_ach_frame();
        pthread_mutex_unlock(&lock);
        struct timespec wait = {0, 16 * 1000 * 1000};
        nanosleep(&wait, NULL);
    }
    return NULL;
}

void runtime_achievements_start(void) {
    if (!session() || connected) return;
    pthread_mutex_lock(&lock);
    const char *version = getenv("SF_VERSION");
    rl_ach_set_info("", version ? version : "");
    connected = rl_ach_init("", NULL, NULL);
    pthread_mutex_unlock(&lock);
    if (!connected) {
        printf("Runtime: Recomp Achievements: RecompLauncher did not answer\n");
        return;
    }
    pthread_t thread;
    if (!pthread_create(&thread, NULL, pump, NULL)) pthread_detach(thread);
    printf("Runtime: Recomp Achievements: connected to RecompLauncher\n");
}

void runtime_achievements_unlock(int id) {
    if (!connected || id < 0) return;
    pthread_mutex_lock(&lock);
    rl_ach_native((uint32_t)id);
    pthread_mutex_unlock(&lock);
}

void runtime_achievements_save_loaded(void) {
    if (!connected) return;
    pthread_mutex_lock(&lock);
    rl_ach_save_loaded();
    pthread_mutex_unlock(&lock);
}

int runtime_achievements_quiet_toasts(void) {
    if (!connected) return 0;
    pthread_mutex_lock(&lock);
    int quiet = rl_ach_quiet_toasts();
    pthread_mutex_unlock(&lock);
    return quiet;
}
