import socket
import sys
import time


def udp_echo_server(host="0.0.0.0", port=2333):
    """
    UDP Echo 服务器
    参数:
        host: 监听地址 (默认所有接口)
        port: 监听端口 (默认 2333)
    """
    try:
        # 创建 UDP socket
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            # 绑定到指定地址和端口
            sock.bind((host, port))
            print(f"UDP Echo 服务器已启动，监听 {host}:{port}")
            print("按 Ctrl+C 停止服务器...")

            while True:
                try:
                    # 接收数据和客户端地址
                    data, addr = sock.recvfrom(1024)  # 1024 字节缓冲区

                    # 解码并打印接收到的消息
                    message = data.decode("utf-8")
                    print(f"收到来自 {addr[0]}:{addr[1]} 的消息: {message}")

                    time.sleep(1)  # 模拟处理延迟

                    # 原样返回消息
                    sock.sendto(data, addr)
                    print(f"已返回 {len(data)} 字节到 {addr[0]}:{addr[1]}")

                except UnicodeDecodeError:
                    # 处理非文本数据
                    print(f"收到来自 {addr} 的 {len(data)} 字节二进制数据")
                    sock.sendto(data, addr)

    except KeyboardInterrupt:
        print("\n服务器已停止")
    except Exception as e:
        print(f"发生错误: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    # 从命令行参数获取端口号(如果提供)
    port = 2333
    if len(sys.argv) > 1:
        try:
            port = int(sys.argv[1])
            if not (0 < port < 65536):
                raise ValueError
        except ValueError:
            print("错误：端口号必须是 1-65535 之间的整数", file=sys.stderr)
            sys.exit(1)

    # 启动服务器
    udp_echo_server(port=port)
