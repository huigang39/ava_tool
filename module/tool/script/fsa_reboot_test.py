import fi_fsa_v3
import time

fsa = fi_fsa_v3.FSA()
fsa.Init("192.168.137.75")


def main():
    fsa.EnableControl(fi_fsa_v3.ctrl_mode_e.VELOCITY_MODE)
    time.sleep(1)

    fsa.SetVelocity(1)
    print("设置速度中...")
    time.sleep(1)

    fsa.Reboot()
    print("重启中...")
    time.sleep(5)

    errcode = fi_fsa_v3.err_code_t()
    fsa.GetErrCode(errcode)

    if errcode.arr[0] & 0x00100000:
        print("高频注入测试失败")
        exit(0)


if __name__ == "__main__":
    for i in range(1000):
        main()
        print(f"已测试{i + 1}轮")
