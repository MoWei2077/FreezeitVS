#pragma once

#include "utils.hpp"
#include "vpopen.hpp"
#include "managedApp.hpp"
#include "doze.hpp"
#include "freezeit.hpp"
#include "systemTools.hpp"

class Freezer {
private:
    Freezeit& freezeit;
    ManagedApp& managedApp;
    SystemTools& systemTools;
    Settings& settings;
    Doze& doze;

    vector<thread> threads;

    WORK_MODE workMode = WORK_MODE::GLOBAL_SIGSTOP;
    map<int, int> pendingHandleList;     //挂起列队 无论黑白名单 { uid, timeRemain:sec }
    set<int> lastForegroundApp;          //前台应用
    set<int> curForegroundApp;           //新前台应用
    set<int> curFgBackup;                //新前台应用备份 用于进入doze前备份， 退出后恢复
    set<int> naughtyApp;                 //冻结期间存在异常解冻或唤醒进程的应用
    mutex naughtyMutex;

    uint32_t timelineIdx = 0;
    uint32_t unfrozenTimeline[4096] = {};
    map<int, uint32_t> unfrozenIdx;

    int refreezeSecRemain = 60; //开机 一分钟时 就压一次
    int remainTimesToRefreshTopApp = 2; //允许多线程冲突，不需要原子操作

    static const size_t GET_VISIBLE_BUF_SIZE = 256 * 1024;
    unique_ptr<char[]> getVisibleAppBuff;

    binder_state bs{ -1, nullptr, 128 * 1024ULL };

    const char* cgroupV2FreezerCheckPath = "/sys/fs/cgroup/uid_0/cgroup.freeze";
    const char* cgroupV2frozenCheckPath = "/sys/fs/cgroup/frozen/cgroup.freeze";       // "1" frozen
    const char* cgroupV2unfrozenCheckPath = "/sys/fs/cgroup/unfrozen/cgroup.freeze";   // "0" unfrozen

    // const char cpusetEventPath[] = "/dev/cpuset/top-app";
    const char* cpusetEventPathA12 = "/dev/cpuset/top-app/tasks";
    const char* cpusetEventPathA13 = "/dev/cpuset/top-app/cgroup.procs";

    const char* cgroupV1FrozenPath = "/dev/jark_freezer/frozen/cgroup.procs";
    const char* cgroupV1UnfrozenPath = "/dev/jark_freezer/unfrozen/cgroup.procs";

    // 如果直接使用 uid_xxx/cgroup.freeze 可能导致无法解冻
    const char* cgroupV2UidPidPath = "/sys/fs/cgroup/uid_%d/pid_%d/cgroup.freeze"; // "1"frozen   "0"unfrozen
    const char* cgroupV2FrozenPath = "/sys/fs/cgroup/frozen/cgroup.procs";         // write pid
    const char* cgroupV2UnfrozenPath = "/sys/fs/cgroup/unfrozen/cgroup.procs";     // write pid


    const char v2wchan[16] = "do_freezer_trap";      // FreezerV2冻结状态
    const char v1wchan[16] = "__refrigerator";       // FreezerV1冻结状态
    const char SIGSTOPwchan[16] = "do_signal_stop";  // SIGSTOP冻结状态
    const char v2xwchan[16] = "get_signal";          // FreezerV2冻结状态 内联状态
    const char pStopwchan[16] = "ptrace_stop";       // ptrace冻结状态
    const char epoll_wait1_wchan[16] = "SyS_epoll_wait";
    const char epoll_wait2_wchan[16] = "do_epoll_wait";
    const char binder_wchan[32] = "binder_ioctl_write_read";
    const char pipe_wchan[16] = "pipe_wait";

public:
    Freezer& operator=(Freezer&&) = delete;

    const string workModeStr(const WORK_MODE mode) {
        const string modeStrList[] = {
                "全局SIGSTOP",
                "FreezerV1 (FROZEN)",
                "FreezerV1 (FRZ+ST)",
                "FreezerV2 (UID)",
                "FreezerV2 (FROZEN)",
                "Unknown" };
        const uint32_t idx = static_cast<uint32_t>(mode);
        return modeStrList[idx <= 5 ? idx : 5];
    }

    Freezer(Freezeit& freezeit, Settings& settings, ManagedApp& managedApp,
        SystemTools& systemTools, Doze& doze) :
        freezeit(freezeit), managedApp(managedApp), systemTools(systemTools),
        settings(settings), doze(doze) {

        getVisibleAppBuff = make_unique<char[]>(GET_VISIBLE_BUF_SIZE);

        //binderInit("/dev/binder");

        threads.emplace_back(thread(&Freezer::cpuSetTriggerTask, this)); //监控前台
        threads.emplace_back(thread(&Freezer::cycleThreadFunc, this));

        checkAndMountV2();
        switch (static_cast<WORK_MODE>(settings.setMode)) {
        case WORK_MODE::GLOBAL_SIGSTOP: {
            workMode = WORK_MODE::GLOBAL_SIGSTOP;
            freezeit.setWorkMode(workModeStr(workMode));
            freezeit.log("已设置[全局SIGSTOP], [Freezer冻结]将变为[SIGSTOP冻结]");
        } return;

        case WORK_MODE::V1: {
            if (mountFreezerV1()) {
                workMode = WORK_MODE::V1;
                freezeit.setWorkMode(workModeStr(workMode));
                freezeit.log("Freezer类型已设为 V1(FROZEN)");
                return;
            }
            freezeit.log("不支持自定义Freezer类型 V1(FROZEN) 失败");
        } break;

        case WORK_MODE::V1_ST: {
            if (mountFreezerV1()) {
                workMode = WORK_MODE::V1_ST;
                freezeit.setWorkMode(workModeStr(workMode));
                freezeit.log("Freezer类型已设为 V1(FRZ+ST)");
                return;
            }
            freezeit.log("不支持自定义Freezer类型 V1(FRZ+ST)");
        } break;

        case WORK_MODE::V2UID: {
            if (checkFreezerV2UID()) {
                workMode = WORK_MODE::V2UID;
                freezeit.setWorkMode(workModeStr(workMode));
                freezeit.log("Freezer类型已设为 V2(UID)");
                return;
            }
            freezeit.log("不支持自定义Freezer类型 V2(UID)");
        } break;

        case WORK_MODE::V2FROZEN: {
            if (checkFreezerV2FROZEN()) {
                workMode = WORK_MODE::V2FROZEN;
                freezeit.setWorkMode(workModeStr(workMode));
                freezeit.log("Freezer类型已设为 V2(FROZEN)");
                return;
            }
            freezeit.log("不支持自定义Freezer类型 V2(FROZEN)");
        } break;
        }

        // 以上手动选择若不支持或失败，下面将进行自动选择
        if (checkFreezerV2FROZEN()) {
            workMode = WORK_MODE::V2FROZEN;
            freezeit.log("Freezer类型已设为 V2(FROZEN)");
        }
        else if (checkFreezerV2UID()) {
            workMode = WORK_MODE::V2UID;
            freezeit.log("Freezer类型已设为 V2(UID)");
        }
        //else if (mountFreezerV1()) { // 部分V1不能内存释放，不再进行自动选择，只能手动选择
        	//workMode = WORK_MODE::V1;
        	//freezeit.log("Freezer类型已设为 V1(FROZEN)");
        //}
        else {
            workMode = WORK_MODE::GLOBAL_SIGSTOP;
            freezeit.log("已开启 [全局SIGSTOP] 冻结模式");
        }
        freezeit.setWorkMode(workModeStr(workMode));
    }

    bool isV1Mode() const {
        return workMode == WORK_MODE::V1_ST || workMode == WORK_MODE::V1;
    }

    void getPids(appInfoStruct& appInfo) {
        START_TIME_COUNT;

        appInfo.pids.clear();

        DIR* dir = opendir("/proc");
        if (dir == nullptr) {
            char errTips[256];
            snprintf(errTips, sizeof(errTips), "错误: %s() [%d]:[%s]", __FUNCTION__, errno,
                strerror(errno));
            fprintf(stderr, "%s", errTips);
            freezeit.log(errTips);
            return;
        }

        struct dirent* file;
        while ((file = readdir(dir)) != nullptr) {
            if (file->d_type != DT_DIR) continue;
            if (file->d_name[0] < '0' || file->d_name[0] > '9') continue;

            const int pid = atoi(file->d_name);
            if (pid <= 100) continue;

            char fullPath[64];
            memcpy(fullPath, "/proc/", 6);
            memcpy(fullPath + 6, file->d_name, 6);

            struct stat statBuf;
            if (stat(fullPath, &statBuf))continue;
            if (statBuf.st_uid != (uid_t)appInfo.uid) continue;

            strcat(fullPath + 8, "/cmdline");
            char readBuff[256];
            if (Utils::readString(fullPath, readBuff, sizeof(readBuff)) == 0)continue;
            const string& package = appInfo.package;
            if (strncmp(readBuff, package.c_str(), package.length())) continue;
            const char endChar = readBuff[package.length()];
            if (endChar != ':' && endChar != 0)continue;

            appInfo.pids.emplace_back(pid);
        }
        closedir(dir);
        END_TIME_COUNT;
    }

    map<int, vector<int>> getRunningPids(set<int>& uidSet) {
        START_TIME_COUNT;
        map<int, vector<int>> pids;

        DIR* dir = opendir("/proc");
        if (dir == nullptr) {
            char errTips[256];
            snprintf(errTips, sizeof(errTips), "错误: %s() [%d]:[%s]", __FUNCTION__, errno,
                strerror(errno));
            fprintf(stderr, "%s", errTips);
            freezeit.log(errTips);
            return pids;
        }

        struct dirent* file;
        while ((file = readdir(dir)) != nullptr) {
            if (file->d_type != DT_DIR) continue;
            if (file->d_name[0] < '0' || file->d_name[0] > '9') continue;

            const int pid = atoi(file->d_name);
            if (pid <= 100) continue;

            char fullPath[64];
            memcpy(fullPath, "/proc/", 6);
            memcpy(fullPath + 6, file->d_name, 6);

            struct stat statBuf;
            if (stat(fullPath, &statBuf))continue;
            const int uid = statBuf.st_uid;
            if (!uidSet.contains(uid))continue;

            strcat(fullPath + 8, "/cmdline");
            char readBuff[256];
            if (Utils::readString(fullPath, readBuff, sizeof(readBuff)) == 0)continue;
            const string& package = managedApp[uid].package;
            if (strncmp(readBuff, package.c_str(), package.length())) continue;

            pids[uid].emplace_back(pid);
        }
        closedir(dir);
        END_TIME_COUNT;
        return pids;
    }

    set<int> getRunningUids(set<int>& uidSet) {
        START_TIME_COUNT;
        set<int> uids;

        DIR* dir = opendir("/proc");
        if (dir == nullptr) {
            char errTips[256];
            snprintf(errTips, sizeof(errTips), "错误: %s() [%d]:[%s]", __FUNCTION__, errno,
                strerror(errno));
            fprintf(stderr, "%s", errTips);
            freezeit.log(errTips);
            return uids;
        }

        struct dirent* file;
        while ((file = readdir(dir)) != nullptr) {
            if (file->d_type != DT_DIR) continue;
            if (file->d_name[0] < '0' || file->d_name[0] > '9') continue;

            const int pid = atoi(file->d_name);
            if (pid <= 100) continue;

            char fullPath[64];
            memcpy(fullPath, "/proc/", 6);
            memcpy(fullPath + 6, file->d_name, 6);

            struct stat statBuf;
            if (stat(fullPath, &statBuf))continue;
            const int uid = statBuf.st_uid;
            if (!uidSet.contains(uid))continue;

            strcat(fullPath + 8, "/cmdline");
            char readBuff[256];
            if (Utils::readString(fullPath, readBuff, sizeof(readBuff)) == 0)continue;
            const string& package = managedApp[uid].package;
            if (strncmp(readBuff, package.c_str(), package.length())) continue;

            uids.insert(uid);
        }
        closedir(dir);
        END_TIME_COUNT;
        return uids;
    }

    void handleSignal(const appInfoStruct& appInfo, const int signal) {
        if (signal == SIGKILL) {
            if (isV1Mode() && appInfo.isFreezeMode())
                handleFreezer(appInfo, false);  // 先给V1解冻， 否则无法杀死

            //先暂停 然后再杀，否则有可能会复活
            for (const auto pid : appInfo.pids)
                kill(pid, SIGSTOP);

            usleep(1000 * 50);
            for (const auto pid : appInfo.pids) {
                kill(pid, SIGKILL);
                //防止毒瘤，休息5秒继续杀
                try
                {
                    usleep(1000 * 1000 * 5);
                    kill(pid, SIGKILL);
                }
                catch (const std::exception&)
                {
                    continue;
                }
            }
            return;
        }

        for (const int pid : appInfo.pids)
            if (kill(pid, signal) < 0 && signal == SIGSTOP)
                freezeit.logFmt("SIGSTOP冻结 [%s PID:%d] 失败[%s]",
                    appInfo.label.c_str(), pid, strerror(errno));
    }

    void handleFreezer(const appInfoStruct& appInfo, const bool freeze) {
        char path[256];

        switch (workMode) {
        case WORK_MODE::V2FROZEN: {
            for (const int pid : appInfo.pids) {
                if (!Utils::writeInt(freeze ? cgroupV2FrozenPath : cgroupV2UnfrozenPath, pid))
                    freezeit.logFmt("%s [%s PID:%d] 失败(V2FROZEN)",
                        freeze ? "冻结" : "解冻", appInfo.label.c_str(), pid);
            }
        } break;

        case WORK_MODE::V2UID: {
            for (const int pid : appInfo.pids) {
                snprintf(path, sizeof(path), cgroupV2UidPidPath, appInfo.uid, pid);
                if (!Utils::writeString(path, freeze ? "1" : "0", 2))
                    freezeit.logFmt("%s [%s PID:%d] 失败(进程可能已结束或者Freezer控制器尚未初始化PID路径)",
                        freeze ? "冻结" : "解冻", appInfo.label.c_str(), pid);
            }
        } break;

        case WORK_MODE::V1_ST: {
            if (freeze) {
                for (const int pid : appInfo.pids) {
                    if (!Utils::writeInt(cgroupV1FrozenPath, pid))
                        freezeit.logFmt("冻结 [%s PID:%d] 失败(V1_ST_F)",
                            appInfo.label.c_str(), pid);
                    if (kill(pid, SIGSTOP) < 0)
                        freezeit.logFmt("冻结 [%s PID:%d] 失败(V1_ST_S)",
                            appInfo.label.c_str(), pid);
                }
            }
            else {
                for (const int pid : appInfo.pids) {
                    if (kill(pid, SIGCONT) < 0)
                        freezeit.logFmt("解冻 [%s PID:%d] 失败(V1_ST_S)",
                            appInfo.label.c_str(), pid);
                    if (!Utils::writeInt(cgroupV1UnfrozenPath, pid))
                        freezeit.logFmt("解冻 [%s PID:%d] 失败(V1_ST_F)",
                            appInfo.label.c_str(), pid);
                }
            }
        } break;

        case WORK_MODE::V1: {
            for (const int pid : appInfo.pids) {
                if (!Utils::writeInt(freeze ? cgroupV1FrozenPath : cgroupV1UnfrozenPath, pid))
                    freezeit.logFmt("%s [%s] 失败(V1) PID:%d",
                        freeze ? "冻结" : "解冻", appInfo.label.c_str(), pid);
            }
        } break;

        // 本函数只处理Freezer模式，其他冻结模式不应来到此处
        default: {
            freezeit.logFmt("%s 使用了错误的冻结模式", appInfo.label.c_str());
        } break;
        }
    }

    // < 0 : 冻结binder失败的pid， > 0 : 冻结成功的进程数
    int handleProcess(appInfoStruct& appInfo, const bool freeze) {
        START_TIME_COUNT;

        if (freeze) {
            getPids(appInfo);
        }
        else {
            erase_if(appInfo.pids, [](const int pid) {
                char path[16];
                snprintf(path, sizeof(path), "/proc/%d", pid);
                return access(path, F_OK);
                });
        }

        switch (appInfo.freezeMode) {
        case FREEZE_MODE::FREEZER: 
        case FREEZE_MODE::FREEZER_BREAK: {
            if (workMode != WORK_MODE::GLOBAL_SIGSTOP) {
                const int res = -1;
                if (res < 0 && freeze && appInfo.isTolerant)
                    return res;
                handleFreezer(appInfo, freeze);
                break;
            }
            // 如果是全局 WORK_MODE::GLOBAL_SIGSTOP 则顺着执行下面
        }

        case FREEZE_MODE::SIGNAL:
        case FREEZE_MODE::SIGNAL_BREAK: {
            const int res = -1;
            if (res < 0 && freeze && appInfo.isTolerant)
                return res;
            handleSignal(appInfo, freeze ? SIGSTOP : SIGCONT);
        } break;

        case FREEZE_MODE::TERMINATE: {
            if (freeze)
                handleSignal(appInfo, SIGKILL);
            return 0;
        }

        default: {
            freezeit.logFmt("不再冻结此应用：%s %s", appInfo.label.c_str(),
                getModeText(appInfo.freezeMode).c_str());
            return 0;
        }
        }

        if (settings.wakeupTimeoutMin != 120) {
            // 无论冻结还是解冻都要清除 解冻时间线上已设置的uid
            auto it = unfrozenIdx.find(appInfo.uid);
            if (it != unfrozenIdx.end())
                unfrozenTimeline[it->second] = 0;

            // 冻结就需要在 解冻时间线 插入下一次解冻的时间
            if (freeze && appInfo.pids.size() && appInfo.isSignalOrFreezer()) {
                uint32_t nextIdx = (timelineIdx + settings.wakeupTimeoutMin * 60) & 0x0FFF; // [ %4096]
                unfrozenIdx[appInfo.uid] = nextIdx;
                unfrozenTimeline[nextIdx] = appInfo.uid;
            }
            else {
                unfrozenIdx.erase(appInfo.uid);
            }
        }

        if (freeze && appInfo.needBreakNetwork()) {
            const auto ret = systemTools.breakNetworkByLocalSocket(appInfo.uid);
            switch (static_cast<REPLY>(ret)) {
            case REPLY::SUCCESS:
                freezeit.logFmt("断网成功: %s", appInfo.label.c_str());
                break;
            case REPLY::FAILURE:
                freezeit.logFmt("断网失败: %s", appInfo.label.c_str());
                break;
            default:
                freezeit.logFmt("断网 未知回应[%d] %s", ret, appInfo.label.c_str());
                break;
            }
        }

        END_TIME_COUNT;
        return appInfo.pids.size();
    }
    // return 0成功  小于0为操作失败的pid
    /*int handleBinder(const appInfoStruct& appInfo, const bool freeze) {
        if (bs.fd <= 0)return 0;

        START_TIME_COUNT;

        // https://cs.android.com/android/kernel/superproject/+/common-android-mainline:common/drivers/android/binder.c;l=5434
        // 100ms 等待传输事务完成
        binder_freeze_info binderInfo{ 0, static_cast<uint32_t>(freeze ? 1 : 0), 100 };
        binder_frozen_status_info statusInfo = { 0, 0, 0 };

        if (freeze) { // 冻结
            binderInfo.enable = 1;
            for (size_t i = 0; i < appInfo.pids.size(); i++) {
                binderInfo.pid = appInfo.pids[i];
                if (ioctl(bs.fd, BINDER_FREEZE, &binderInfo) < 0) {
                    int errorCode = errno;

                    // ret == EAGAIN indicates that transactions have not drained.
                    // Call again to poll for completion.
                    if (errorCode != EAGAIN)
                        freezeit.logFmt("冻结 Binder 发生异常 [%s:%u] ErrorCode:%d", appInfo.label.c_str(), binderInfo.pid, errorCode);

                    // 解冻已经被冻结binder的进程
                    binderInfo.enable = 0;
                    for (size_t j = 0; j < i; j++) {
                        binderInfo.pid = appInfo.pids[j];

                        //TODO 如果解冻失败？
                        if (ioctl(bs.fd, BINDER_FREEZE, &binderInfo) < 0) {
                            errorCode = errno;
                            freezeit.logFmt("撤消冻结：解冻恢复Binder发生错误：[%s:%u] ErrorCode:%d", appInfo.label.c_str(), binderInfo.pid, errorCode);
                        }
                    }
                    return -appInfo.pids[i];
                }
            }

            usleep(1000 * 200);

            for (size_t i = 0; i < appInfo.pids.size(); i++) {
                statusInfo.pid = appInfo.pids[i];
                if (ioctl(bs.fd, BINDER_GET_FROZEN_INFO, &statusInfo) < 0) {
                    int errorCode = errno;
                    freezeit.logFmt("获取 [%s:%d] Binder 状态错误 ErrroCode:%d", appInfo.label.c_str(), statusInfo.pid, errorCode);
                }
                else if (statusInfo.sync_recv & 2) { // 冻结后发现仍有传输事务
                    freezeit.logFmt("%s 仍有Binder传输事务", appInfo.label.c_str());

                    // 解冻已经被冻结binder的进程
                    binderInfo.enable = 0;
                    for (size_t j = 0; j < i; j++) {
                        binderInfo.pid = appInfo.pids[j];

                        //TODO 如果解冻失败？
                        if (ioctl(bs.fd, BINDER_FREEZE, &binderInfo) < 0) {
                            int errorCode = errno;
                            freezeit.logFmt("撤消冻结：解冻恢复Binder发生错误：[%s:%u] ErrorCode:%d", appInfo.label.c_str(), binderInfo.pid, errorCode);
                        }
                    }
                    return -appInfo.pids[i];
                }

            }
        }
        else { // 解冻
            for (size_t i = 0; i < appInfo.pids.size(); i++) {
                statusInfo.pid = appInfo.pids[i];
                if (ioctl(bs.fd, BINDER_GET_FROZEN_INFO, &statusInfo) < 0) {
                    int errorCode = errno;
                    freezeit.logFmt("获取 [%s:%d] Binder 状态错误 ErrroCode:%d", appInfo.label.c_str(), statusInfo.pid, errorCode);
                }
                else {
                    // 注意各个二进制位差别
                    // https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/jni/com_android_server_am_CachedAppOptimizer.cpp;l=489
                    // https://cs.android.com/android/kernel/superproject/+/common-android-mainline:common/drivers/android/binder.c;l=5467
                    if (statusInfo.sync_recv & 1) {
                        freezeit.logFmt("%s 冻结期间存在 同步传输 Sync transactions, 考虑杀掉进程", appInfo.label.c_str());
                        //TODO 要杀掉进程
                    }
                    if (statusInfo.async_recv & 1) {
                        freezeit.logFmt("%s 冻结期间存在 异步传输（不重要）", appInfo.label.c_str());
                    }
                    if (statusInfo.sync_recv & 2) {
                        freezeit.logFmt("%s 冻结期间存在“未完成”传输（不重要）TXNS_PENDING", appInfo.label.c_str());
                    }
                }
            }

            binderInfo.enable = 0;
            for (size_t i = 0; i < appInfo.pids.size(); i++) {
                binderInfo.pid = appInfo.pids[i];
                if (ioctl(bs.fd, BINDER_FREEZE, &binderInfo) < 0) {
                    int errorCode = errno;
                    freezeit.logFmt("解冻 Binder 发生异常 [%s:%u] ErrorCode:%d", appInfo.label.c_str(), binderInfo.pid, errorCode);

                    char tmp[32];
                    snprintf(tmp, sizeof(tmp), "/proc/%d", binderInfo.pid);

                    if (access(tmp, F_OK)) {
                        freezeit.logFmt("进程已不在 [%s] %u", appInfo.label.c_str(), binderInfo.pid);
                    }
                    //TODO 再解冻一次，若失败，考虑杀死？
                    else if (ioctl(bs.fd, BINDER_FREEZE, &binderInfo) < 0) {
                        errorCode = errno;
                        freezeit.logFmt("重试解冻 Binder 发生异常 [%s:%u] ErrorCode:%d", appInfo.label.c_str(), binderInfo.pid, errorCode);
                    }
                }
            }
        }

        END_TIME_COUNT;
        return 0;
    }
    */
    // 重新压制第三方。 白名单, 前台, 待冻结列队 都跳过
    void checkReFreeze() {
        START_TIME_COUNT;

        if (--refreezeSecRemain > 0) return;

        refreezeSecRemain = settings.getRefreezeTimeout();

        lock_guard<mutex> lock(naughtyMutex);

        if (naughtyApp.size() == 0) {
            DIR* dir = opendir("/proc");
            if (dir == nullptr) {
                char errTips[256];
                snprintf(errTips, sizeof(errTips), "错误: %s() [%d]:[%s]", __FUNCTION__, errno,
                    strerror(errno));
                fprintf(stderr, "%s", errTips);
                freezeit.log(errTips);
                return;
            }

            struct dirent* file;
            while ((file = readdir(dir)) != nullptr) {
                if (file->d_type != DT_DIR) continue;
                if (file->d_name[0] < '0' || file->d_name[0] > '9') continue;

                const int pid = atoi(file->d_name);
                if (pid <= 100) continue;

                char fullPath[64];
                memcpy(fullPath, "/proc/", 6);
                memcpy(fullPath + 6, file->d_name, 6);

                struct stat statBuf;
                if (stat(fullPath, &statBuf))continue;
                const int uid = statBuf.st_uid;
                if (!managedApp.contains(uid) || pendingHandleList.contains(uid) || curForegroundApp.contains(uid))
                    continue;

                auto& appInfo = managedApp[uid];
                if (appInfo.isWhitelist())
                    continue;

                strcat(fullPath + 8, "/cmdline");
                char readBuff[256];
                if (Utils::readString(fullPath, readBuff, sizeof(readBuff)) == 0)continue;
                if (strncmp(readBuff, appInfo.package.c_str(), appInfo.package.length())) continue;

                memcpy(fullPath + 6, file->d_name, 6);
                strcat(fullPath + 8, "/wchan");
                if (Utils::readString(fullPath, readBuff, sizeof(readBuff)) == 0)continue;
                if (strcmp(readBuff, v2wchan) && strcmp(readBuff, v1wchan) && strcmp(readBuff, SIGSTOPwchan) && 
                    strcmp(readBuff, v2xwchan) && strcmp(readBuff, pStopwchan)) {
                    naughtyApp.insert(uid);
                }
            }
            closedir(dir);
        }

        stackString<1024> tmp("定时压制");
        for (const auto uid : naughtyApp) {
            pendingHandleList[uid] = 1;
            tmp.append(' ').append(managedApp[uid].label.c_str());
        }
        if (naughtyApp.size()) {
            naughtyApp.clear();
            freezeit.log(tmp.c_str());
        }
        else {
            freezeit.log("定时压制 目前均处于冻结状态");
        }

        END_TIME_COUNT;
    }

    bool mountFreezerV1() {
        if (!access("/dev/jark_freezer", F_OK)) // 已挂载
            return true;

        // https://man7.org/linux/man-pages/man7/cgroups.7.html
        // https://www.kernel.org/doc/Documentation/cgroup-v1/freezer-subsystem.txt
        // https://www.containerlabs.kubedaily.com/LXC/Linux%20Containers/The-cgroup-freezer-subsystem.html

        mkdir("/dev/jark_freezer", 0666);
        mount("freezer", "/dev/jark_freezer", "cgroup", 0, "freezer");
        usleep(1000 * 100);
        mkdir("/dev/jark_freezer/frozen", 0666);
        mkdir("/dev/jark_freezer/unfrozen", 0666);
        usleep(1000 * 100);
        Utils::writeString("/dev/jark_freezer/frozen/freezer.state", "FROZEN");
        Utils::writeString("/dev/jark_freezer/unfrozen/freezer.state", "THAWED");

        // https://www.spinics.net/lists/cgroups/msg24540.html
        // https://android.googlesource.com/device/google/crosshatch/+/9474191%5E%21/
        Utils::writeString("/dev/jark_freezer/frozen/freezer.killable", "1"); // 旧版内核不支持
        usleep(1000 * 100);

        return (!access(cgroupV1FrozenPath, F_OK) && !access(cgroupV1UnfrozenPath, F_OK));
    }

    bool checkFreezerV2UID() {
        return (!access(cgroupV2FreezerCheckPath, F_OK));
    }

    bool checkFreezerV2FROZEN() {
        return (!access(cgroupV2frozenCheckPath, F_OK) && !access(cgroupV2unfrozenCheckPath, F_OK));
    }

    void checkAndMountV2() {
        // https://cs.android.com/android/kernel/superproject/+/common-android12-5.10:common/kernel/cgroup/freezer.c

        if (checkFreezerV2UID())
            freezeit.log("原生支持 FreezerV2(UID)");

        if (checkFreezerV2FROZEN()) {
            freezeit.log("原生支持 FreezerV2(FROZEN)");
        }
        else {
            mkdir("/sys/fs/cgroup/frozen/", 0666);
            mkdir("/sys/fs/cgroup/unfrozen/", 0666);
            usleep(1000 * 500);

            if (checkFreezerV2FROZEN()) {
                auto fd = open(cgroupV2frozenCheckPath, O_WRONLY | O_TRUNC);
                if (fd > 0) {
                    write(fd, "1", 2);
                    close(fd);
                }
                freezeit.logFmt("设置%s FreezerV2(FROZEN)", fd > 0 ? "成功" : "失败");

                fd = open(cgroupV2unfrozenCheckPath, O_WRONLY | O_TRUNC);
                if (fd > 0) {
                    write(fd, "0", 2);
                    close(fd);
                }
                freezeit.logFmt("设置%s FreezerV2(UNFROZEN)", fd > 0 ? "成功" : "失败");

                freezeit.log("现已支持 FreezerV2(FROZEN)");
            }
        }
    }

    void printProcState() {
        START_TIME_COUNT;

        DIR* dir = opendir("/proc");
        if (dir == nullptr) {
            freezeit.logFmt("错误: %s(), [%d]:[%s]\n", __FUNCTION__, errno, strerror(errno));
            return;
        }

        int getSignalCnt = 0;
        int totalMiB = 0;
        set<int> uidSet, pidSet;

        lock_guard<mutex> lock(naughtyMutex);
        naughtyApp.clear();

        stackString<1024 * 16> stateStr("进程冻结状态:\n\n PID | MiB |  状 态  | 进 程\n");

        struct dirent* file;
        while ((file = readdir(dir)) != nullptr) {
            if (file->d_type != DT_DIR) continue;
            if (file->d_name[0] < '0' || file->d_name[0] > '9') continue;

            const int pid = atoi(file->d_name);
            if (pid <= 100) continue;

            char fullPath[64];
            memcpy(fullPath, "/proc/", 6);
            memcpy(fullPath + 6, file->d_name, 6);

            struct stat statBuf;
            if (stat(fullPath, &statBuf))continue;
            const int uid = statBuf.st_uid;
            if (!managedApp.contains(uid)) continue;

            auto& appInfo = managedApp[uid];
            if (appInfo.isWhitelist()) continue;

            strcat(fullPath + 8, "/cmdline");
            char readBuff[256]; // now is cmdline Content
            if (Utils::readString(fullPath, readBuff, sizeof(readBuff)) == 0)continue;
            if (strncmp(readBuff, appInfo.package.c_str(), appInfo.package.length())) continue;

            uidSet.insert(uid);
            pidSet.insert(pid);

            stackString<256> label(appInfo.label.c_str(), appInfo.label.length());
            if (readBuff[appInfo.package.length()] == ':')
                label.append(readBuff + appInfo.package.length());

            memcpy(fullPath + 6, file->d_name, 6);
            strcat(fullPath + 8, "/statm");
            Utils::readString(fullPath, readBuff, sizeof(readBuff)); // now is statm content
            const char* ptr = strchr(readBuff, ' ');

            // Unit: 1 page(4KiB) convert to MiB. (atoi(ptr) * 4 / 1024)
            const int memMiB = ptr ? (atoi(ptr + 1) >> 8) : 0;
            totalMiB += memMiB;

            if (curForegroundApp.contains(uid)) {
                stateStr.appendFmt("%5d %4d 📱正在前台 %s\n", pid, memMiB, label.c_str());
                continue;
            }

            if (pendingHandleList.contains(uid)) {
                stateStr.appendFmt("%5d %4d ⏳%d秒后冻结 %s\n", pid, memMiB, pendingHandleList[uid], label.c_str());
                continue;
            }

            memcpy(fullPath + 6, file->d_name, 6);
            strcat(fullPath + 8, "/wchan");
            if (Utils::readString(fullPath, readBuff, sizeof(readBuff)) == 0) {
                uidSet.erase(uid);
                pidSet.erase(pid);
                continue;
            }

            stateStr.appendFmt("%5d %4d ", pid, memMiB);
            if (!strcmp(readBuff, v2wchan)) {
                stateStr.appendFmt("❄️V2冻结中 %s\n", label.c_str());
            }
            else if (!strcmp(readBuff, v1wchan)) {
                stateStr.appendFmt("❄️V1冻结中 %s\n", label.c_str());
            }
            else if (!strcmp(readBuff, SIGSTOPwchan)) {
                stateStr.appendFmt("🧊ST冻结中 %s\n", label.c_str());
            }
            else if (!strcmp(readBuff, v2xwchan)) {
                stateStr.appendFmt("❄️V2*冻结中 %s\n", label.c_str());
                getSignalCnt++;
            }
            else if (!strcmp(readBuff, pStopwchan)) {
                stateStr.appendFmt("🧊ST冻结中(ptrace_stop) %s\n", label.c_str());
            }
            else if (!strcmp(readBuff, binder_wchan)) {
                stateStr.appendFmt("⚠️运行中(Binder通信) %s\n", label.c_str());
                naughtyApp.insert(uid);
            }
            else if (!strcmp(readBuff, pipe_wchan)) {
                stateStr.appendFmt("⚠️运行中(管道通信) %s\n", label.c_str());
                naughtyApp.insert(uid);
            }
            else if (!strcmp(readBuff, epoll_wait1_wchan) || !strcmp(readBuff, epoll_wait2_wchan)) {
                stateStr.appendFmt("⚠️运行中(就绪态) %s\n", label.c_str());
                naughtyApp.insert(uid);
            }
            else {
                stateStr.appendFmt("⚠️运行中(%s) %s\n", (const char*)readBuff, label.c_str());
                naughtyApp.insert(uid);
            }
        }
        closedir(dir);

        if (uidSet.size() == 0) {
            freezeit.log("设为冻结的应用没有运行");
        }
        else {

            if (naughtyApp.size()) {
                stateStr.append("\n ⚠️ 发现 [未冻结] 的进程, 即将进行冻结 ⚠️\n", 65);
                refreezeSecRemain = 0;
            }

            stateStr.appendFmt("\n总计 %d 应用 %d 进程, 占用内存 ", (int)uidSet.size(), (int)pidSet.size());
            stateStr.appendFmt("%.2f GiB", totalMiB / 1024.0);
            if (getSignalCnt)
                stateStr.append(", V2*带星号状态为get_signal，小概率非冻结状态");
            if (isV1Mode())
                stateStr.append(", V1已冻结状态可能会识别为[运行中]，请到[CPU使用时长]页面查看是否跳动", 98);

            freezeit.log(stateStr.c_str());
        }
        END_TIME_COUNT;
    }

    // 解冻新APP, 旧APP加入待冻结列队 call once per 0.5 sec when Touching
    void updateAppProcess() {
        vector<int> newShowOnApp, toBackgroundApp;

        for (const int uid : curForegroundApp)
            if (!lastForegroundApp.contains(uid))
                newShowOnApp.emplace_back(uid);

        for (const int uid : lastForegroundApp)
            if (!curForegroundApp.contains(uid))
                toBackgroundApp.emplace_back(uid);

        if (newShowOnApp.size() || toBackgroundApp.size())
            lastForegroundApp = curForegroundApp;
        else
            return;

        for (const int uid : newShowOnApp) {
            // 如果在待冻结列表则只需移除
            if (pendingHandleList.erase(uid))
                continue;

            // 更新[打开时间]  并解冻
            auto& appInfo = managedApp[uid];
            appInfo.startTimestamp = time(nullptr);

            const int num = handleProcess(appInfo, false);
            if (num > 0) freezeit.logFmt("☀️解冻 %s %d进程", appInfo.label.c_str(), num);
            else freezeit.logFmt("😁打开 %s", appInfo.label.c_str());
        }

        for (const int uid : toBackgroundApp) // 更新倒计时
            pendingHandleList[uid] = managedApp[uid].isTerminateMode() ? 
            settings.terminateTimeout : settings.freezeTimeout;
    }

    // 处理待冻结列队 call once per 1sec
    void processPendingApp() {
        auto it = pendingHandleList.begin();
        while (it != pendingHandleList.end()) {
            auto& remainSec = it->second;
            if (--remainSec > 0) {//每次轮询减一
                it++;
                continue;
            }

            const int uid = it->first;
            auto& appInfo = managedApp[uid];
            const int num = handleProcess(appInfo, true);
            if (num < 0) {
                remainSec = 15 << (++appInfo.failFreezeCnt);
                freezeit.logFmt("%s:%d Binder正在传输, 延迟 %d%s 再冻结", appInfo.label.c_str(), -num,
                    remainSec < 60 ? remainSec : remainSec / 60, remainSec < 60 ? "秒" : "分");
                it++;
                continue;
            }
            it = pendingHandleList.erase(it);
            appInfo.failFreezeCnt = 0;

            appInfo.stopTimestamp = time(nullptr);
            const int delta = appInfo.startTimestamp == 0 ? 0:
                (appInfo.stopTimestamp - appInfo.startTimestamp);
            appInfo.startTimestamp = appInfo.stopTimestamp;
            appInfo.totalRunningTime += delta;
            const int total = appInfo.totalRunningTime;

            stackString<128> timeStr("运行");
            if (delta >= 3600)
                timeStr.appendFmt("%d时", delta / 3600);
            if (delta >= 60)
                timeStr.appendFmt("%d分", (delta % 3600) / 60);
            timeStr.appendFmt("%d秒", delta % 60);

            timeStr.append(" 累计", 7);
            if (total >= 3600)
                timeStr.appendFmt("%d时", total / 3600);
            if (total >= 60)
                timeStr.appendFmt("%d分", (total % 3600) / 60);
            timeStr.appendFmt("%d秒", total % 60);

            if (num)
                freezeit.logFmt("%s冻结 %s %d进程 %s",
                    appInfo.isSignalMode() ? "🧊" : "❄️",
                    appInfo.label.c_str(), num, timeStr.c_str());
            else freezeit.logFmt("😭关闭 %s %s", appInfo.label.c_str(), *timeStr);
        }
    }

    void checkWakeup() {
        timelineIdx = (timelineIdx + 1) & 0x0FFF; // [ %4096]
        const auto uid = unfrozenTimeline[timelineIdx];
        if (uid == 0) return;

        unfrozenTimeline[timelineIdx] = 0;//清掉时间线当前位置UID信息

        if (!managedApp.contains(uid)) return;

        auto& appInfo = managedApp[uid];
        if (appInfo.isSignalOrFreezer()) {
            const int num = handleProcess(appInfo, false);
            if (num > 0) {
                appInfo.startTimestamp = time(nullptr);
                pendingHandleList[uid] = settings.freezeTimeout;//更新待冻结倒计时
                freezeit.logFmt("☀️定时解冻 %s %d进程", appInfo.label.c_str(), num);
            }
            else {
                freezeit.logFmt("🗑️后台被杀 %s", appInfo.label.c_str());
            }
        }
        else {
            unfrozenIdx.erase(uid);
        }
    }


    // 常规查询前台 只返回第三方, 剔除白名单/桌面
    void getVisibleAppByShell() {
        START_TIME_COUNT;

        curForegroundApp.clear();
        const char* cmdList[] = { "/system/bin/cmd", "cmd", "activity", "stack", "list", nullptr };
        VPOPEN::vpopen(cmdList[0], cmdList + 1, getVisibleAppBuff.get(), GET_VISIBLE_BUF_SIZE);

        stringstream ss;
        ss << getVisibleAppBuff.get();

        // 以下耗时仅为 VPOPEN::vpopen 的 2% ~ 6%
        string line;
        while (getline(ss, line)) {
            if (!managedApp.hasHomePackage() && line.find("mActivityType=home") != string::npos) {
                getline(ss, line); //下一行就是桌面信息
                auto startIdx = line.find_last_of('{');
                auto endIdx = line.find_last_of('/');
                if (startIdx == string::npos || endIdx == string::npos || startIdx > endIdx)
                    continue;

                managedApp.updateHomePackage(line.substr(startIdx + 1, endIdx - (startIdx + 1)));
            }

            //  taskId=8655: com.ruanmei.ithome/com.ruanmei.ithome.ui.MainActivity bounds=[0,1641][1440,3200]
            //     userId=0 visible=true topActivity=ComponentInfo{com.ruanmei.ithome/com.ruanmei.ithome.ui.NewsInfoActivity}
            if (!line.starts_with("  taskId=")) continue;
            if (line.find("visible=true") == string::npos) continue;

            auto startIdx = line.find_last_of('{');
            auto endIdx = line.find_last_of('/');
            if (startIdx == string::npos || endIdx == string::npos || startIdx > endIdx) continue;

            const string& package = line.substr(startIdx + 1, endIdx - (startIdx + 1));
            if (!managedApp.contains(package)) continue;
            int uid = managedApp.getUid(package);
            if (managedApp[uid].isWhitelist()) continue;
            curForegroundApp.insert(uid);
        }

        if (curForegroundApp.size() >= (lastForegroundApp.size() + 3)) //有时系统会虚报大量前台应用
            curForegroundApp = lastForegroundApp;

        END_TIME_COUNT;
    }

    // 常规查询前台 只返回第三方, 剔除白名单/桌面
    void getVisibleAppByShellLRU(set<int>& cur) {
        START_TIME_COUNT;

        cur.clear();
        const char* cmdList[] = { "/system/bin/dumpsys", "dumpsys", "activity", "lru", nullptr };
        VPOPEN::vpopen(cmdList[0], cmdList + 1, getVisibleAppBuff.get(), GET_VISIBLE_BUF_SIZE);

        stringstream ss;
        ss << getVisibleAppBuff.get();

        // 以下耗时仅 0.08-0.14ms, VPOPEN::vpopen 15-60ms
        string line;
        getline(ss, line);

        bool isHook = strncmp(line.c_str(), "JARK006_LRU", 4) == 0;
        /*
      Hook
      OnePlus6:/ # dumpsys activity lru
      JARK006_LRU
      10XXX 2
      10XXX 3
      */
        if (isHook) {
            while (getline(ss, line)) {
                if (strncmp(line.c_str(), "10", 2))continue;

                int uid, level;
                sscanf(line.c_str(), "%d %d", &uid, &level);
                if (level < 2 || 6 < level) continue;

                if (!managedApp.contains(uid))continue;
                if (managedApp[uid].isWhitelist())continue;
                if ((level <= 3) || managedApp[uid].isTolerant) cur.insert(uid);
#if DEBUG_DURATION
                freezeit.logFmt("Hook前台 %s:%d", managedApp[uid].label.c_str(), level);
#endif
            }
        }
        else if (freezeit.SDK_INT_VER >= 29) { //Android 11 Android 12+

            /* SDK 31-32-33
            OnePlus6:/ # dumpsys activity lru
            ACTIVITY MANAGER LRU PROCESSES (dumpsys activity lru)
              Activities:
              #45: cch+ 5 CEM  ---- 5537:com.tencent.mobileqq/u0a212
              Other:
              #39: svcb   SVC  ---- 19270:com.tencent.mm/u0a221

            generic_x86_64:/ $ getprop ro.build.version.sdk
            30
            generic_x86_64:/ $ dumpsys activity lru
            ACTIVITY MANAGER LRU PROCESSES (dumpsys activity lru)
              Activities:
              #30: fg     TOP  LCM 995:com.android.launcher3/u0a117 act:activities|recents
              Other:
              #29: cch+ 5 CEM  --- 801:com.android.permissioncontroller/u0a127
              # 6: pers   PER  LCM 1354:com.android.ims.rcsservice/1001
              # 5: psvc   PER  LCM 670:com.android.bluetooth/1002

            !!! !!! !!!

            generic_x86_64:/ $ getprop ro.build.version.sdk
            29
            generic_x86_64:/ # dumpsys activity lru
            ACTIVITY MANAGER LRU PROCESSES (dumpsys activity lru)
              Activities:
                #26: fore   TOP  2961:com.android.launcher3/u0a100  activity=activities|recents
              Other:
                #25: cch+ 5 CEM  3433:com.android.dialer/u0a101
                #24: prev   LAST 3349:android.process.acore/u0a52
                #23: cch+ 5 CEM  4100:com.android.keychain/1000
                #9: cch+75 CEM  3551:com.android.managedprovisioning/u0a59
                #8: prcp   IMPB 2601:com.android.inputmethod.latin/u0a115
            */
            auto getForegroundLevel = [](const char* ptr) {
                // const char level[][8] = {
                // // 0, 1,   2顶层,   3, 4常驻状态栏, 5, 6悬浮窗
                // "PER ", "PERU", "TOP ", "BTOP", "FGS ", "BFGS", "IMPF",
                // };
                // for (int i = 2; i < sizeof(level) / sizeof(level[0]); i++) {
                //   if (!strncmp(ptr, level[i], 4))
                //     return i;
                // }

                constexpr uint32_t levelInt[7] = { 0x20524550, 0x55524550, 0x20504f54, 0x504f5442,
                                                  0x20534746, 0x53474642, 0x46504d49 };
                const uint32_t target = *((uint32_t*)ptr);
                for (int i = 2; i < 7; i++) {
                    if (target == levelInt[i])
                        return i;
                }
                return 16;
            };

            int offset = freezeit.SDK_INT_VER == 29 ? 5 : 3; // 行首 空格加#号 数量
            auto startStr = freezeit.SDK_INT_VER == 29 ? "    #" : "  #";
            getline(ss, line);
            if (!strncmp(line.c_str(), "  Activities:", 4)) {
                while (getline(ss, line)) {
                    // 此后每行必需以 "  #"、"    #" 开头，否则就是 Service: Other:需跳过
                    if (strncmp(line.c_str(), startStr, offset)) break;

                    auto linePtr = line.c_str() + offset; // 偏移已经到数字了

                    auto ptr = linePtr + (linePtr[2] == ':' ? 11 : 12); //11: # 1 ~ 99   12: #100+
                    int level = getForegroundLevel(ptr);
                    if (level < 2 || 6 < level) continue;

                    ptr = strstr(line.c_str(), "/u0a");
                    if (!ptr)continue;
                    int uid = 10000 + atoi(ptr + 4);
                    if (!managedApp.contains(uid))continue;
                    if (managedApp[uid].isWhitelist())continue;
                    if ((level <= 3) || managedApp[uid].isTolerant) cur.insert(uid);

#if DEBUG_DURATION
                    freezeit.logFmt("Legacy前台 %s:%d", managedApp[uid].label.c_str(), level);
#endif
                }
            }
        }
        END_TIME_COUNT;
    }

    void getVisibleAppByLocalSocket() {
        START_TIME_COUNT;

        int buff[64];
        int recvLen = Utils::localSocketRequest(XPOSED_CMD::GET_FOREGROUND, nullptr, 0, buff,
            sizeof(buff));

        int& UidLen = buff[0];
        if (recvLen <= 0) {
            freezeit.logFmt("%s() 工作异常, 请确认LSPosed中冻它勾选系统框架, 然后重启", __FUNCTION__);
            END_TIME_COUNT;
            return;
        }
        else if (UidLen > 16 || (UidLen != (recvLen / 4 - 1))) {
            freezeit.logFmt("%s() 前台服务数据异常 UidLen[%d] recvLen[%d]", __FUNCTION__, UidLen, recvLen);
            freezeit.logFmt("DumpHex: %s", Utils::bin2Hex(buff, recvLen < 64 * 4 ? recvLen : 64 * 4).c_str());
            END_TIME_COUNT;
            return;
        }

        curForegroundApp.clear();
        for (int i = 1; i <= UidLen; i++) {
            int& uid = buff[i];
            if (managedApp.contains(uid)) curForegroundApp.insert(uid);
            else freezeit.logFmt("非法UID[%d], 可能是新安装的应用, 请点击右上角第一个按钮更新应用列表", uid);
        }

#if DEBUG_DURATION
        string tmp;
        for (auto& uid : curForegroundApp)
            tmp += " [" + managedApp[uid].label + "]";
        if (tmp.length())
            freezeit.logFmt("LOCALSOCKET前台%s", tmp.c_str());
        else
            freezeit.log("LOCALSOCKET前台 空");
#endif
        END_TIME_COUNT;
    }


    string getModeText(FREEZE_MODE mode) {
        switch (mode) {
        case FREEZE_MODE::TERMINATE:
            return "杀死后台";
        case FREEZE_MODE::SIGNAL:
            return "SIGSTOP冻结";
        case FREEZE_MODE::SIGNAL_BREAK:
            return "SIGSTOP冻结断网";
        case FREEZE_MODE::FREEZER:
            return "Freezer冻结";
        case FREEZE_MODE::FREEZER_BREAK:
            return "Freezer冻结断网";
        case FREEZE_MODE::WHITELIST:
            return "自由后台";
        case FREEZE_MODE::WHITEFORCE:
            return "自由后台(内置)";
        default:
            return "未知";
        }
    }

    void eventTouchTriggerTask(int n) {
        constexpr int TRIGGER_BUF_SIZE = 8192;

        char touchEventPath[64];
        snprintf(touchEventPath, sizeof(touchEventPath), "/dev/input/event%d", n);

        usleep(n * 1000 * 10);

        int inotifyFd = inotify_init();
        if (inotifyFd < 0) {
            fprintf(stderr, "同步事件: 0xA%d (1/3)失败: [%d]:[%s]", n, errno, strerror(errno));
            exit(-1);
        }

        int watch_d = inotify_add_watch(inotifyFd, touchEventPath, IN_ALL_EVENTS);
        if (watch_d < 0) {
            fprintf(stderr, "同步事件: 0xA%d (2/3)失败: [%d]:[%s]", n, errno, strerror(errno));
            exit(-1);
        }

        freezeit.logFmt("初始化同步事件: 0xA%d", n);

        constexpr int REMAIN_TIMES_MAX = 2;
        char buf[TRIGGER_BUF_SIZE];
        while (read(inotifyFd, buf, TRIGGER_BUF_SIZE) > 0) {
            remainTimesToRefreshTopApp = REMAIN_TIMES_MAX;
            usleep(500 * 1000);
        }

        inotify_rm_watch(inotifyFd, watch_d);
        close(inotifyFd);

        freezeit.logFmt("已退出监控同步事件: 0xA%d", n);
    }

    void cpuSetTriggerTask() {
        constexpr int TRIGGER_BUF_SIZE = 8192;

        sleep(1);

        int inotifyFd = inotify_init();
        if (inotifyFd < 0) {
            fprintf(stderr, "同步事件: 0xB0 (1/3)失败: [%d]:[%s]", errno, strerror(errno));
            exit(-1);
        }

        int watch_d = inotify_add_watch(inotifyFd,
            freezeit.SDK_INT_VER >= 33 ? cpusetEventPathA13
            : cpusetEventPathA12,
            IN_ALL_EVENTS);

        if (watch_d < 0) {
            fprintf(stderr, "同步事件: 0xB0 (2/3)失败: [%d]:[%s]", errno, strerror(errno));
            exit(-1);
        }

        freezeit.log("初始化同步事件: 0xB0");

        constexpr int REMAIN_TIMES_MAX = 2;
        char buf[TRIGGER_BUF_SIZE];
        while (read(inotifyFd, buf, TRIGGER_BUF_SIZE) > 0) {
            remainTimesToRefreshTopApp = REMAIN_TIMES_MAX;
            usleep(500 * 1000);
        }

        inotify_rm_watch(inotifyFd, watch_d);
        close(inotifyFd);

        freezeit.log("已退出监控同步事件: 0xB0");
    }

    void cycleThreadFunc() {
        uint32_t halfSecondCnt{ 0 };

        sleep(1);
        getVisibleAppByShell(); // 获取桌面

        while (true) {
            usleep(500 * 1000);

            if (remainTimesToRefreshTopApp > 0) {
                remainTimesToRefreshTopApp--;
                START_TIME_COUNT;
                if (doze.isScreenOffStandby) {
                    if (doze.checkIfNeedToExit()) {
                        curForegroundApp = std::move(curFgBackup); // recovery
                        updateAppProcess();
                        setWakeupLockByLocalSocket(WAKEUP_LOCK::DEFAULT);
                    }
                }
                else {
#ifdef __x86_64__
                    getVisibleAppByShellLRU(curForegroundApp);
#else
                    getVisibleAppByLocalSocket();
#endif
                    updateAppProcess(); // ~40us
                }
                END_TIME_COUNT;
            }

            if (++halfSecondCnt & 1) continue;

            systemTools.cycleCnt++;

            processPendingApp();//1秒一次

            // 2分钟一次 在亮屏状态检测是否已经息屏  息屏状态则检测是否再次强制进入深度Doze
            if (doze.checkIfNeedToEnter()) {
                curFgBackup = std::move(curForegroundApp); //backup
                updateAppProcess();
                setWakeupLockByLocalSocket(WAKEUP_LOCK::IGNORE);
            }

            if (doze.isScreenOffStandby)continue;// 息屏状态 不用执行 以下功能

            systemTools.checkBattery();// 1分钟一次 电池检测
            checkReFreeze();// 重新压制切后台的应用
            checkWakeup();// 检查是否有定时解冻
        }
    }


    void getBlackListUidRunning(set<int>& uids) {
        uids.clear();

        START_TIME_COUNT;

        DIR* dir = opendir("/proc");
        if (dir == nullptr) {
            char errTips[256];
            snprintf(errTips, sizeof(errTips), "错误: %s() [%d]:[%s]", __FUNCTION__, errno,
                strerror(errno));
            fprintf(stderr, "%s", errTips);
            freezeit.log(errTips);
            return;
        }

        struct dirent* file;
        while ((file = readdir(dir)) != nullptr) {
            if (file->d_type != DT_DIR) continue;
            if (file->d_name[0] < '0' || file->d_name[0] > '9') continue;

            const int pid = atoi(file->d_name);
            if (pid <= 100) continue;

            char fullPath[64];
            memcpy(fullPath, "/proc/", 6);
            memcpy(fullPath + 6, file->d_name, 6);

            struct stat statBuf;
            if (stat(fullPath, &statBuf))continue;
            const int uid = statBuf.st_uid;
            if (!managedApp.contains(uid) || managedApp[uid].isWhitelist())
                continue;

            strcat(fullPath + 8, "/cmdline");
            char readBuff[256];
            if (Utils::readString(fullPath, readBuff, sizeof(readBuff)) == 0)continue;
            const auto& package = managedApp[uid].package;
            if (strncmp(readBuff, package.c_str(), package.length())) continue;

            uids.insert(uid);
        }
        closedir(dir);
        END_TIME_COUNT;
    }

    int setWakeupLockByLocalSocket(const WAKEUP_LOCK mode) {
        static set<int> blackListUidRunning;
        START_TIME_COUNT;

        if (mode == WAKEUP_LOCK::IGNORE)
            getBlackListUidRunning(blackListUidRunning);

        if (blackListUidRunning.empty())return 0;

        int buff[64] = { static_cast<int>(blackListUidRunning.size()), static_cast<int>(mode) };
        int i = 2;
        for (const int uid : blackListUidRunning)
            buff[i++] = uid;

        const int recvLen = Utils::localSocketRequest(XPOSED_CMD::SET_WAKEUP_LOCK, buff,
            i * sizeof(int), buff, sizeof(buff));

        if (recvLen == 0) {
            freezeit.logFmt("%s() 工作异常, 请确认LSPosed中冻它勾选系统框架, 然后重启", __FUNCTION__);
            END_TIME_COUNT;
            return 0;
        }
        else if (recvLen != 4) {
            freezeit.logFmt("%s() 返回数据异常 recvLen[%d]", __FUNCTION__, recvLen);
            if (recvLen > 0 && recvLen < 64 * 4)
                freezeit.logFmt("DumpHex: %s", Utils::bin2Hex(buff, recvLen).c_str());
            END_TIME_COUNT;
            return 0;
        }
        END_TIME_COUNT;
        return buff[0];
    }

    // https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/am/CachedAppOptimizer.java;l=753
    // https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/jni/com_android_server_am_CachedAppOptimizer.cpp;l=475
    // https://cs.android.com/android/platform/superproject/main/+/main:frameworks/native/libs/binder/IPCThreadState.cpp;l=1564
    // https://cs.android.com/android/kernel/superproject/+/common-android-mainline:common/drivers/android/binder.c;l=5615
    // https://elixir.bootlin.com/linux/latest/source/drivers/android/binder.c#L5412



    /*void binder_close() {
        munmap(bs.mapped, bs.mapSize);
        close(bs.fd);
        bs.fd = -1;
    }
    */
    /*void binderInit(const char* driver) {

        bs.fd = open(driver, O_RDWR | O_CLOEXEC);
        if (bs.fd < 0) {
            freezeit.logFmt("Binder初始化失败 路径打开失败：[%s] [%d:%s]", driver, errno, strerror(errno));
            if (freezeit.kernelVersion.main <= 4) { // 4.xx 内核不支持
                freezeit.logFmt("内核版本低(%d.%d.%d)，不支持 BINDER_FREEZER 特性",
                    freezeit.kernelVersion.main, freezeit.kernelVersion.sub, freezeit.kernelVersion.patch);
                return;
            }
            return;
        }

        struct binder_version b_ver { -1 };
        if ((ioctl(bs.fd, BINDER_VERSION, &b_ver) < 0) ||
            (b_ver.protocol_version != BINDER_CURRENT_PROTOCOL_VERSION)) {
            freezeit.logFmt("Binder初始化失败 binder版本要求: %d  本机版本: %d", BINDER_CURRENT_PROTOCOL_VERSION,
                b_ver.protocol_version);
            close(bs.fd);
            bs.fd = -1;
            return;
        }
        else {
            freezeit.logFmt("初始驱动 BINDER协议版本 %d", b_ver.protocol_version);
        }

        // https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/jni/com_android_server_am_CachedAppOptimizer.cpp;l=489
        binder_frozen_status_info info = { (uint32_t)getpid(), 0, 0 };
        if (ioctl(bs.fd, BINDER_GET_FROZEN_INFO, &info) < 0) {
            int ret = -errno;
            freezeit.logFmt("Binder初始化失败 不支持 BINDER_FREEZER 特性 ErrroCode:%d", ret);
            close(bs.fd);
            bs.fd = -1;
            return;
        }
        else {
            freezeit.log("特性支持 BINDER_FREEZER");
        }

        bs.mapped = mmap(NULL, bs.mapSize, PROT_READ, MAP_PRIVATE, bs.fd, 0);
        if (bs.mapped == MAP_FAILED) {
            freezeit.logFmt("Binder初始化失败 Binder mmap失败 [%s] [%d:%s]", driver, errno, strerror(errno));
            close(bs.fd);
            bs.fd = -1;
            return;
        }
    }*/
    
};
