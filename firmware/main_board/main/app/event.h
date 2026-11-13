#ifndef EVENT_H
#define EVENT_H

/*
 * Events, delivered at once to the sounds (sound.c, robots/<name>.json
 * "sounds.events") and to every running feature's on_event.
 */
enum {
    EV_WAKE,              /* voice: the wake word (xiaozhi interrupts the duck while it speaks) */
    EV_WAKE_IDLE,         /* voice: woken, with no command words to wait for (xiaozhi starts a conversation) */
    EV_POLICIES_CHANGED,  /* a policy was imported or removed */
    EV_COMMAND_OK,        /* voice: a command ran */
    EV_COMMAND_REJECTED,  /* voice: a command was refused */
    EV_LISTEN_TIMEOUT,    /* voice or xiaozhi: stopped listening */
    EV_SKILL_DONE,        /* app.c, from the robot state: a skill finished */
    EV_FALL,              /* app.c: the duck fell with torque on */
    EV_LOW_BATTERY,       /* app.c: the battery went below low_battery_v */
    EV_ESTOP,             /* app.c: torque went off */
    EV_COUNT,
};

void event_post(int ev, const char *text);

#endif
