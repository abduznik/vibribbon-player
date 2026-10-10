"""PS1 BIOS function tables (from the psx-spx documentation), A0/B0/C0."""
_A0 = """FileOpen FileSeek FileRead FileWrite FileClose FileIoctl exit FileGetDeviceFlag FileGetc FilePutc
todigit atof strtoul strtol abs labs atoi atol atob setjmp longjmp strcat strncat strcmp strncmp strcpy strncpy
strlen index rindex strchr strrchr strpbrk strspn strcspn strtok strstr toupper tolower bcopy bzero bcmp memcpy
memset memmove memcmp memchr rand srand qsort strtod malloc free lsearch bsearch calloc realloc InitHeap
SystemErrorExit std_in_getchar std_out_putchar std_in_gets std_out_puts printf SystemErrorUnresolvedException
LoadExeHeader LoadExeFile DoExecute FlushCache init_a0_b0_c0_vectors GPU_dw gpu_send_dma SendGP1Command GPU_cw
GPU_cwp send_gpu_linked_list gpu_abort_dma GetGPUStatus gpu_sync a4f a50 LoadAndExecute a52 a53 CdInit_a54 _bu_init_a55
CdRemove_a56 a57 a58 a59 a5a dev_tty_init dev_tty_open dev_tty_in_out dev_tty_ioctl dev_cd_open dev_cd_read dev_cd_close
dev_cd_firstfile dev_cd_nextfile dev_cd_chdir dev_card_open dev_card_read dev_card_write dev_card_close dev_card_firstfile
dev_card_nextfile dev_card_erase dev_card_undelete dev_card_format dev_card_rename dev_card_6f _bu_init _96_init _96_remove
a73 a74 a75 a76 a77 CdAsyncSeekL a79 a7a a7b CdAsyncGetStatus a7d CdAsyncReadSector a7f a80 CdAsyncSetMode""".split()
_B0 = """alloc_kernel_memory free_kernel_memory init_timer get_timer enable_timer_irq disable_timer_irq restart_timer
DeliverEvent OpenEvent CloseEvent WaitEvent TestEvent EnableEvent DisableEvent OpenThread CloseThread ChangeThread
jump_to_00000000h InitPad StartPad StopPad PAD_init PAD_dr ReturnFromException ResetEntryInt HookEntryInt b1a b1b b1c b1d b1e b1f
UnDeliverEvent b21 b22 b23 b24 b25 b26 b27 b28 b29 b2a b2b b2c b2d b2e b2f b30 b31
FileOpen FileSeek FileRead FileWrite FileClose FileIoctl exit FileGetDeviceFlag FileGetc FilePutc std_in_getchar
std_out_putchar std_in_gets std_out_puts chdir format firstfile nextfile rename erase undelete AddDrv DelDrv
PrintInstalledDevices InitCard StartCard StopCard _card_info_subfunc write_card_sector read_card_sector allow_new_card
Krom2RawAdd SystemError Krom2Offset GetLastError GetLastFileError GetC0Table GetB0Table get_bu_callback_port testdevice b5a
ChangeClearPad get_card_status wait_card_status""".split()
_C0 = """EnqueueTimerAndVblankIrqs EnqueueSyscallHandler SysEnqIntRP SysDeqIntRP get_free_EvCB_slot get_free_TCB_slot
ExceptionHandler InstallExceptionHandlers SysInitMemory SysInitKernelVariables ChangeClearRCnt SystemError InitDefInt
SetIrqAutoAck c0e c0f c10 c11 InstallDevices FlushStdInOutPut c14 tty_cdevinput tty_cdevscan tty_circgetc tty_circputc
ioabort set_card_find_mode KernelRedirect AdjustA0Table get_card_find_mode""".split()
A0 = dict(enumerate(_A0)); B0 = dict(enumerate(_B0)); C0 = dict(enumerate(_C0))
TABLES = {0xA0: A0, 0xB0: B0, 0xC0: C0}
