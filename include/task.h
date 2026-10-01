#ifndef GUARD_TASK_H
#define GUARD_TASK_H

#define HEAD_SENTINEL 0xFE
#define TAIL_SENTINEL 0xFF
#define TASK_NONE TAIL_SENTINEL

#define NUM_TASKS 16

// Several subsystems reinterpret Task::data as a larger struct (ListMenu is the
// worst case, and STATIC_ASSERT in list_menu.c enforces the bound). On the GBA
// every pointer is 4 bytes; on a 64-bit host they are 8, so the same structs
// need proportionally more room. Grow the data array to match, which keeps the
// aliasing behaviour identical -- only the amount of scratch space differs.
#if defined(PLATFORM_NATIVE)
#define NUM_TASK_DATA 28
#else
#define NUM_TASK_DATA 16
#endif

typedef void (*TaskFunc)(u8 taskId);

struct Task
{
    TaskFunc func;
    bool8 isActive;
    u8 prev;
    u8 next;
    u8 priority;
    s16 data[NUM_TASK_DATA];
};

extern struct Task gTasks[];

void ResetTasks(void);
u8 CreateTask(TaskFunc func, u8 priority);
void DestroyTask(u8 taskId);
void RunTasks(void);
void TaskDummy(u8 taskId);
void SetTaskFuncWithFollowupFunc(u8 taskId, TaskFunc func, TaskFunc followupFunc);
void SwitchTaskToFollowupFunc(u8 taskId);
bool8 FuncIsActiveTask(TaskFunc func);
u8 FindTaskIdByFunc(TaskFunc func);
u8 GetTaskCount(void);
void SetWordTaskArg(u8 taskId, u8 dataElem, u32 value);
u32 GetWordTaskArg(u8 taskId, u8 dataElem);

#endif // GUARD_TASK_H
