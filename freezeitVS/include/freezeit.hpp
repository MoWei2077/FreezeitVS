#pragma once

#include "utils.hpp"
#include "vpopen.hpp"

class Freezeit {
private:
    const char* LOG_PATH = "/sdcard/Android/freezeit.log";

    constexpr static int LINE_SIZE = 1024 * 32;   //  32 KiB
    constexpr static int BUFF_SIZE = 1024 * 128;  // 128 KiB

    mutex logPrintMutex;
    bool toFileFlag = false;
    size_t position = 0;
    char lineCache[LINE_SIZE] = "[00:00:00]  ";
    char logCache[BUFF_SIZE];

    string propPath;
    string changelog{ "无" };

    map<string, string> prop{
            {"id",          "Unknown"},
            {"name",        "Unknown"},
            {"version",     "Unknown"},
            {"versionCode", "0"},
            {"author",      "Unknown"},
            {"description", "Unknown"},
    };

    // "Jul 28 2022" --> "2022-07-28"
    const char compilerDate[12] = {
            __DATE__[7],
            __DATE__[8],
            __DATE__[9],
            __DATE__[10],// YYYY year
            '-',

            // First month letter, Oct Nov Dec = '1' otherwise '0'
            (__DATE__[0] == 'O' || __DATE__[0] == 'N' || __DATE__[0] == 'D') ? '1' : '0',

            // Second month letter Jan, Jun or Jul
            (__DATE__[0] == 'J') ? ((__DATE__[1] == 'a') ? '1'
            : ((__DATE__[2] == 'n') ? '6' : '7'))
            : (__DATE__[0] == 'F') ? '2'// Feb
            : (__DATE__[0] == 'M') ? (__DATE__[2] == 'r') ? '3' : '5'// Mar or May
            : (__DATE__[0] == 'A') ? (__DATE__[1] == 'p') ? '4' : '8'// Apr or Aug
            : (__DATE__[0] == 'S') ? '9'// Sep
            : (__DATE__[0] == 'O') ? '0'// Oct
            : (__DATE__[0] == 'N') ? '1'// Nov
            : (__DATE__[0] == 'D') ? '2'// Dec
            : 'X',

            '-',
            __DATE__[4] == ' ' ? '0' : __DATE__[4],// First day letter, replace space with digit
            __DATE__[5],// Second day letter
        '\0',
    };
    //加密信息有关作者全部更改为JARK006/XShe或酷安@JARK006/酷安@XShee
    /* python3
    def infoEncrypt():
        bytes1 = "冻它模块 JARK006".encode('utf-8')
        bytes2 = bytes(b ^ 0x91 for b in bytes1)
        print('uint8_t authorInfo[{}] = {{  //  ^0x91'.format(len(bytes2)+1), end='')
        cnt = 0
        for byte in bytes2:
            if cnt % 16 == 0:
                print('\n    ', end='')
            cnt += 1
            print("0x%02x, " % byte, end='')
        print('\n    0x91,  //特殊结束符\n};')
        bytes3 = bytes(b ^ 0x91 for b in bytes2)
        print('[', bytes3.decode('utf-8'), ']')
    infoEncrypt()
    */

    // "冻它模块 By:JARK006" 简单加密  byte ^= 0x91
    uint8_t authorInfo[26] = {  //  ^0x91
    0x74, 0x17, 0x2a, 0x74, 0x3f, 0x12, 0x77, 0x39, 0x30, 0x74, 0x0c, 0x06, 0xb1, 0xdb, 0xd0, 0xc3,
    0xda, 0xa1, 0xa1, 0xa7, 0xbe, 0xc9, 0xc2, 0xf9, 0xf4,
    0x91,  //特殊结束符
    };

    /*
    checkInfo = [
        "freezeit",
        "冻它",
        "JARK006",
        "id",
        "name",
        "author",
        "description",
    ]
    maxLen = 0
    for str in checkInfo:
        maxLen = max(maxLen, len(str))
    maxLen = int(maxLen / 4 + 1) * 4
    print("\nuint8_t checkInfo[%d][%d] = {" % (len(checkInfo), maxLen))
    for str in checkInfo:
        bytes1 = bytes(b ^ 0x91 for b in str.encode('utf-8'))
        print('  { ', end='')
        for byte in bytes1:
            print("0x%02x, " % byte, end='')
        print('0x91, ', end='')
        for i in range(maxLen - len(bytes1) - 1):
            print('0x%02x, ' % randint(0, 255), end='')
        print('}, //', str)
    print('};\n')
    */
    uint8_t checkInfo[7][16] = {
  { 0xf7, 0xe3, 0xf4, 0xf4, 0xeb, 0xf4, 0xf8, 0xe5, 0x91, 0xd3, 0x8d, 0xd8, 0x04, 0x72, 0x5d, 0x56, }, // freezeit
  { 0x74, 0x17, 0x2a, 0x74, 0x3f, 0x12, 0x91, 0xba, 0xb2, 0x75, 0xef, 0xde, 0xdd, 0x15, 0xa6, 0x0f, }, // 冻它
  { 0xdb, 0xd0, 0xc3, 0xda, 0xa1, 0xa1, 0xa7, 0xbe, 0xc9, 0xc2, 0xf9, 0xf4, 0x91, 0x57, 0x6f, 0x5d, }, // JARK006/XShe
  { 0xf8, 0xf5, 0x91, 0x76, 0x49, 0x74, 0x8c, 0xc2, 0x12, 0x9b, 0x6b, 0xd8, 0xa2, 0x98, 0xb1, 0xef, }, // id
  { 0xff, 0xf0, 0xfc, 0xf4, 0x91, 0xba, 0x09, 0xe2, 0x0f, 0x67, 0x10, 0xb2, 0xb4, 0xb4, 0xf6, 0x3f, }, // name
  { 0xf0, 0xe4, 0xe5, 0xf9, 0xfe, 0xe3, 0x91, 0x45, 0xbb, 0x03, 0xba, 0x4c, 0x3b, 0xad, 0x64, 0x66, }, // author
  { 0xf5, 0xf4, 0xe2, 0xf2, 0xe3, 0xf8, 0xe1, 0xe5, 0xf8, 0xfe, 0xff, 0x91, 0x90, 0x67, 0x7f, 0x23, }, // description
    };

    /*
    def infoEncrypt():
        bytes1 = "模块属性已被篡改, 原版模块为[冻它模块:freezeit], 原创开发者: 酷安@JARK006。"\
        "篡改后属性: ID[%s] 名称[%s] 作者[%s], 本模块已停止工作".encode('utf-8')
        bytes2 = bytes(b ^ 0x91 for b in bytes1)
        print('uint8_t tipsFormat[{}] = {{  //  ^0x91'.format(len(bytes2)+1), end='')
        cnt = 0
        for byte in bytes2:
            if cnt % 16 == 0:
                print('\n    ', end='')
            cnt += 1
            print("0x%02x, " % byte, end='')
        print('\n    0x91,  //特殊结束符\n};')
        bytes3 = bytes(b ^ 0x91 for b in bytes2)
        print('[', bytes3.decode('utf-8'), ']')
    infoEncrypt()
    */
    uint8_t tipsFormat[199] = {  //  ^0x91
    0x77, 0x39, 0x30, 0x74, 0x0c, 0x06, 0x74, 0x20, 0x0f, 0x77, 0x11, 0x36, 0x74, 0x26, 0x23, 0x79,
    0x33, 0x3a, 0x76, 0x3e, 0x30, 0x77, 0x05, 0x28, 0xbd, 0xb1, 0x74, 0x1f, 0x0e, 0x76, 0x18, 0x19,
    0x77, 0x39, 0x30, 0x74, 0x0c, 0x06, 0x75, 0x29, 0x2b, 0xca, 0x74, 0x17, 0x2a, 0x74, 0x3f, 0x12,
    0x77, 0x39, 0x30, 0x74, 0x0c, 0x06, 0xab, 0xf7, 0xe3, 0xf4, 0xf4, 0xeb, 0xf4, 0xf8, 0xe5, 0xcc,
    0xbd, 0xb1, 0x74, 0x1f, 0x0e, 0x74, 0x19, 0x0a, 0x74, 0x2d, 0x11, 0x74, 0x1e, 0x00, 0x79, 0x11,
    0x14, 0xab, 0xb1, 0x78, 0x14, 0x26, 0x74, 0x3f, 0x18, 0xd1, 0xdb, 0xd0, 0xc3, 0xda, 0xa1, 0xa1,
    0xa7, 0x7e, 0x2d, 0x1d, 0x77, 0x05, 0x28, 0x76, 0x18, 0x19, 0x75, 0x2c, 0x0d, 0x79, 0x11, 0x14,
    0xab, 0x78, 0x14, 0x26, 0x74, 0x3f, 0x18, 0xd1, 0xc9, 0xc2, 0xf9, 0xf4, 0xf4, 0xf4, 0xf4, 0x76,
    0x3e, 0x30, 0x77, 0x05, 0x28, 0x74, 0x01, 0x1f, 0x74, 0x20, 0x0f, 0x77, 0x11, 0x36, 0xab, 0xb1,
    0xd8, 0xd5, 0xca, 0xb4, 0xe2, 0xcc, 0xb1, 0x74, 0x01, 0x1c, 0x76, 0x36, 0x21, 0xca, 0xb4, 0xe2,
    0xcc, 0xb1, 0x75, 0x2c, 0x0d, 0x79, 0x11, 0x14, 0xca, 0xb4, 0xe2, 0xcc, 0xbd, 0xb1, 0x77, 0x0d,
    0x3d, 0x77, 0x39, 0x30, 0x74, 0x0c, 0x06, 0x74, 0x26, 0x23, 0x74, 0x10, 0x0d, 0x77, 0x3c, 0x33,
    0x74, 0x26, 0x34, 0x75, 0x2c, 0x0d,
    0x91,  //特殊结束符
    };

    /*
    def infoEncrypt():
      bytes1 = "/sdcard/Android/本机安装过【冻它模块】的非法篡改版.txt".encode('utf-8')
      bytes2 = bytes(b ^ 0x91 for b in bytes1)
      print('uint8_t warnPath[{}] = {{  //  ^0x91'.format(len(bytes2)+1), end='')
      cnt = 0
      for byte in bytes2:
          if cnt % 16 == 0:
              print('\n    ', end='')
          cnt += 1
          print("0x%02x, " % byte, end='')
      print('\n    0x91,  //特殊结束符\n};')
      bytes3 = bytes(b ^ 0x91 for b in bytes2)
      print('[', bytes3.decode('utf-8'), ']')
    infoEncrypt()
    */
    uint8_t warnPath[72] = {  //  ^0x91
    0xbe, 0xe2, 0xf5, 0xf2, 0xf0, 0xe3, 0xf5, 0xbe, 0xd0, 0xff, 0xf5, 0xe3, 0xfe, 0xf8, 0xf5, 0xbe,
    0x77, 0x0d, 0x3d, 0x77, 0x0d, 0x2b, 0x74, 0x3f, 0x18, 0x79, 0x32, 0x14, 0x79, 0x2e, 0x16, 0x72,
    0x11, 0x01, 0x74, 0x17, 0x2a, 0x74, 0x3f, 0x12, 0x77, 0x39, 0x30, 0x74, 0x0c, 0x06, 0x72, 0x11,
    0x00, 0x76, 0x0b, 0x15, 0x78, 0x0c, 0x0f, 0x77, 0x22, 0x04, 0x76, 0x3e, 0x30, 0x77, 0x05, 0x28,
    0x76, 0x18, 0x19, 0xbf, 0xe5, 0xe9, 0xe5,
    0x91,  //特殊结束符
    };

    void toMem(const char* logStr, const int len) {
        if ((position + len) >= BUFF_SIZE)
            position = 0;

        memcpy(logCache + position, logStr, len);
        position += len;
    }

    void toFile(const char* logStr, const int len) {
        auto fp = fopen(LOG_PATH, "ab");
        if (!fp) {
            fprintf(stderr, "日志输出(追加模式)失败 [%d][%s]", errno, strerror(errno));
            return;
        }

        auto fileSize = ftell(fp);
        if ((fileSize + len) >= BUFF_SIZE) {
            fclose(fp);
            usleep(1000);
            fp = fopen(LOG_PATH, "wb");
            if (!fp) {
                fprintf(stderr, "日志输出(超额清理模式)失败 [%d][%s]", errno, strerror(errno));
                return;
            }
        }

        fwrite(logStr, 1, len, fp);
        fclose(fp);
    }

public:

    bool isSamsung{ false };
    bool isOppoVivo{ false };

    int ANDROID_VER = 0;
    int SDK_INT_VER = 0;
    KernelVersionStruct kernelVersion;

    string modulePath;
    string moduleEnv{ "Unknown" };
    string workMode{ "Unknown" };
    string kernelVerStr{ "Unknown" };
    string androidVerStr{ "Unknown" };

    uint32_t extMemorySize{ 0 };

    Freezeit& operator=(Freezeit&&) = delete;

    Freezeit(int argc, const string& fullPath) {

        Utils::myDecode(checkInfo, sizeof(checkInfo));

        if (!fullPath.ends_with((const char*)checkInfo[0])) {
            fprintf(stderr, "路径不正确 [%s]", fullPath.c_str());
            exit(-1);
        }

        modulePath = Utils::parentDir(fullPath);

        int versionCode = -1;
        if (!access("/system/bin/magisk", F_OK)) {
            moduleEnv = "Magisk";
            versionCode = MAGISK::get_version_code();
            if (versionCode <= 0) {
                sleep(2);
                versionCode = MAGISK::get_version_code();
            }
        }
        else if (!access("/data/adb/ksud", F_OK)) {
            moduleEnv = "KernelSU";
            versionCode = KSU::get_version_code();
            if (versionCode <= 0) {
                sleep(2);
                versionCode = KSU::get_version_code();
            }
        }
        if (versionCode > 0)
            moduleEnv += " (" + to_string(versionCode) + ")";

        auto fp = fopen((modulePath + "/boot.log").c_str(), "rb");
        if (fp) {
            auto len = fread(logCache, 1, BUFF_SIZE, fp);
            if (len > 0)
                position = len;
            fclose(fp);
        }

        toFileFlag = argc > 1;
        if (toFileFlag) {
            if (position)toFile(logCache, position);
            const char tips[] = "日志已通过文件输出: /sdcard/Android/freezeit.log";
            toMem(tips, sizeof(tips) - 1);
        }

        propPath = modulePath + "/module.prop";
        fp = fopen(propPath.c_str(), "r");
        if (!fp) {
            fprintf(stderr, "找不到模块属性文件 [%s]", propPath.c_str());
            exit(-1);
        }

        char tmp[1024*4];
        while (!feof(fp)) {
            fgets(tmp, sizeof(tmp), fp);
            if (!isalpha(tmp[0])) continue;
            tmp[sizeof(tmp) - 1] = 0;
            auto ptr = strchr(tmp, '=');
            if (!ptr)continue;

            *ptr = 0;
            for (size_t i = (ptr - tmp) + 1; i < sizeof(tmp); i++) {
                if (tmp[i] == '\n' || tmp[i] == '\r') {
                    tmp[i] = 0;
                    break;
                }
            }
            prop[string(tmp)] = string(ptr + 1);
        }
        fclose(fp);

        checkModuleProp();

        changelog = Utils::readString((modulePath + "/changelog.txt").c_str());

        Utils::myDecode(authorInfo, sizeof(authorInfo));
        logFmt("%s", authorInfo);
        Utils::myDecode(authorInfo, sizeof(authorInfo));

        logFmt("模块版本 %s", prop["version"].c_str());
        logFmt("编译时间 %s %s UTC+8", compilerDate, __TIME__);

        fprintf(stderr, "version %s", prop["version"].c_str()); // 发送当前版本信息给监控进程

        ANDROID_VER = __system_property_get("ro.build.version.release", tmp) > 0 ? atoi(tmp) : 0;
        SDK_INT_VER = __system_property_get("ro.build.version.sdk", tmp) > 0 ? atoi(tmp) : 0;
        androidVerStr = to_string(ANDROID_VER) + " (API " + to_string(SDK_INT_VER) + ")";

        utsname kernelInfo{};
        if (!uname(&kernelInfo)) {
            sscanf(kernelInfo.release, "%d.%d.%d", &kernelVersion.main, &kernelVersion.sub,
                &kernelVersion.patch);
            kernelVerStr =
                to_string(kernelVersion.main) + "." + to_string(kernelVersion.sub) + "." +
                to_string(kernelVersion.patch);
            logFmt("内核版本 %d.%d.%d", kernelVersion.main, kernelVersion.sub, kernelVersion.patch);
        }
        else {
            log("内核版本 获取异常");
        }

        char res[256];
        if (__system_property_get("gsm.operator.alpha", res) > 0 && res[0] != ',')
            logFmt("运营信息 %s", res);
        if (__system_property_get("gsm.network.type", res) > 0) logFmt("网络类型 %s", res);
        if (__system_property_get("ro.product.brand", res) > 0) {
            logFmt("设备厂商 %s", res);

            //for (int i = 0; i < 8; i++)res[i] |= 32;
            *((uint64_t*)res) |= 0x20202020'20202020ULL; // 转为小写
            if (!strncmp(res, "samsung", 7))
                isSamsung = true;
            else if (!strncmp(res, "oppo", 4) || !strncmp(res, "vivo", 4) ||
                !strncmp(res, "realme", 6) || !strncmp(res, "iqoo", 4))
                isOppoVivo = true;
        }
        if (__system_property_get("ro.product.marketname", res) > 0) logFmt("设备型号 %s", res);
        if (__system_property_get("persist.sys.device_name", res) > 0) logFmt("设备名称 %s", res);
        if (__system_property_get("ro.system.build.version.incremental", res) > 0)
            logFmt("系统版本 %s", res);
        if (__system_property_get("ro.soc.manufacturer", res) > 0 &&
            __system_property_get("ro.soc.model", res + 100) > 0)
            logFmt("硬件平台 %s %s", res, res + 100);
    }

    char* getChangelogPtr() { return (char*)changelog.c_str(); }

    size_t getChangelogLen() { return changelog.length(); }

    void checkModuleProp() {
        if (prop[string((const char*)checkInfo[3])].starts_with((const char*)checkInfo[0]) &&
            prop[string((const char*)checkInfo[4])].starts_with((const char*)checkInfo[1]) &&
            prop[string((const char*)checkInfo[5])].starts_with((const char*)checkInfo[2]))
            return;

        char tips[1024];
        Utils::myDecode(tipsFormat, sizeof(tipsFormat));
        const int len = snprintf(tips, sizeof(tips), (const char*)tipsFormat,
            prop[string((const char*)checkInfo[3])].c_str(),
            prop[string((const char*)checkInfo[4])].c_str(),
            prop[string((const char*)checkInfo[5])].c_str());
        Utils::myDecode(tipsFormat, sizeof(tipsFormat));

        Utils::myDecode(warnPath, sizeof(warnPath));
        auto fd = open((const char*)warnPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd > 0) {
            write(fd, tips, len);
            close(fd);
        }
        Utils::myDecode(warnPath, sizeof(warnPath));

        prop[string((const char*)checkInfo[6])] = string(tips);
        if (!saveProp()) fprintf(stderr, "写入 [moduleInfo] 失败");
        fprintf(stderr, "%s", tips);
        exit(-10);
    }

    bool saveProp() {
        auto fp = fopen(propPath.c_str(), "wb");
        if (!fp)
            return false;

        char tmp[1024];
        size_t len = snprintf(tmp, sizeof(tmp),
            "id=%s\nname=%s\nversion=%s\nversionCode=%s\nauthor=%s\ndescription=%s\nupdateJson=%s\n",
            prop["id"].c_str(), prop["name"].c_str(), prop["version"].c_str(),
            prop["versionCode"].c_str(),
            prop["author"].c_str(), prop["description"].c_str(),
            prop["updateJson"].c_str());

        size_t writeLen = fwrite(tmp, 1, len, fp);
        fclose(fp);

        return (writeLen == len);
    }

    void setWorkMode(const string& mode) {
        workMode = mode;
    }

    size_t formatProp(char* ptr, const size_t maxSize, const int cpuCluster) {
        return snprintf(ptr, maxSize, "%s\n%s\n%s\n%s\n%s\n%d\n%s\n%s\n%s\n%s\n%u",
            prop["id"].c_str(), prop["name"].c_str(), prop["version"].c_str(),
            prop["versionCode"].c_str(), prop["author"].c_str(),
            cpuCluster, moduleEnv.c_str(), workMode.c_str(),
            androidVerStr.c_str(), kernelVerStr.c_str(), extMemorySize);
    }

    void log(const string& logContent) {
        log(logContent.c_str());
    }
    void formatTime() {
        time_t timeStamp = time(nullptr) + 8 * 3600L;
        int hour = (timeStamp / 3600) % 24;
        int min = (timeStamp % 3600) / 60;
        int sec = timeStamp % 60;

        //lineCache[LINE_SIZE] = "[00:00:00]  ";
        lineCache[1] = (hour / 10) + '0';
        lineCache[2] = (hour % 10) + '0';
        lineCache[4] = (min / 10) + '0';
        lineCache[5] = (min % 10) + '0';
        lineCache[7] = (sec / 10) + '0';
        lineCache[8] = (sec % 10) + '0';

    }
    void log(const char* str) {
        lock_guard<mutex> lock(logPrintMutex);

        formatTime();

        auto len = strlen(str);
        memcpy(lineCache + 11, str, len);
        len += 11;

        lineCache[len++] = '\n';

        if (toFileFlag)
            toFile(lineCache, len);
        else
            toMem(lineCache, len);
    }

    template<typename... Args>
    void logFmt(const char* fmt, Args&&... args) {
        lock_guard<mutex> lock(logPrintMutex);

        formatTime();

        int len = snprintf(lineCache + 11, (size_t)(LINE_SIZE - 11), fmt, std::forward<Args>(args)...) + 11;

        if (len <= 11 || LINE_SIZE <= (len + 1)) {
            lineCache[11] = 0;
            fprintf(stderr, "日志异常: len[%d] lineCache[%s]", len, lineCache);
            return;
        }

        lineCache[len++] = '\n';

        if (toFileFlag)
            toFile(lineCache, len);
        else
            toMem(lineCache, len);
    }

    void clearLog() {
        logCache[0] = '\n';
        position = 1;
    }

    char* getLogPtr() {
        return logCache;
    }

    size_t getLoglen() {
        return position;
    }
};
