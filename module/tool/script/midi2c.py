import mido
import math


# MIDI 音符编号转物理频率 (Hz)
def midi_to_freq(note):
    if note == 0:
        return 0.0
    # 国际标准音高公式: f = 440 * 2^((p-69)/12)
    return 440.0 * math.pow(2.0, (note - 69) / 12.0)


def convert_midi_to_c(midi_file_path, output_c_file):
    try:
        mid = mido.MidiFile(midi_file_path)
    except Exception as e:
        print(f"读取 MIDI 文件失败: {e}")
        return

    # 准备生成 C 代码的字符串
    c_code = "/* 由 Python 脚本自动生成的 MIDI 旋律数组 */\n"
    c_code += "#include <stdint.h>\n\n"
    c_code += "typedef struct {\n"
    c_code += "    float frequency;     // 频率 (Hz)\n"
    c_code += "    uint32_t duration_ms; // 持续时间 (毫秒)\n"
    c_code += "} Note_t;\n\n"
    c_code += "const Note_t melody[] = {\n"

    current_note = 0
    time_accum_s = 0.0  # 累计时间 (秒)

    # 遍历 MIDI 消息 (mido 迭代时 msg.time 是以秒为单位的 delta time)
    for msg in mid:
        time_accum_s += msg.time

        # 遇到按下琴键 (note_on 且力度大于 0)
        if msg.type == 'note_on' and msg.velocity > 0:
            # 如果按键之前有一段空白时间，记为休止符
            if time_accum_s > 0.005:  # 忽略 5ms 以内的微小误差
                c_code += f"    {{0.0f, {int(time_accum_s * 1000)}}}, \t// 休止符\n"
            current_note = msg.note
            time_accum_s = 0.0  # 时间清零，开始计算这个音符的时长

        # 遇到松开琴键 (note_off 或者力度为 0 的 note_on)
        elif msg.type == 'note_off' or (msg.type == 'note_on' and msg.velocity == 0):
            if msg.note == current_note:
                freq = midi_to_freq(current_note)
                duration = int(time_accum_s * 1000)
                c_code += f"    {{{freq:.2f}f, {duration}}}, \t// 音符 {current_note}\n"
                current_note = 0
                time_accum_s = 0.0

    # 添加一个结束标志位，方便 MCU 判断播放完毕
    c_code += "    {0.0f, 0} \t// 结束标志\n"
    c_code += "};\n"

    # 写入文件
    with open(output_c_file, 'w', encoding='utf-8') as f:
        f.write(c_code)

    print(f"搞定！已成功生成 C 语言数组文件: {output_c_file}")


# --- 使用示例 ---
if __name__ == "__main__":
    # 替换成你的 MIDI 文件路径
    input_midi = "test_melody.mid"
    output_c = "melody_data.h"

    convert_midi_to_c(input_midi, output_c)
