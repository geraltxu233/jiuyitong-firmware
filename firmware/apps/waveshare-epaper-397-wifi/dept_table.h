// 由 tools/gen_dept_table.py 生成，不要手改。
// 来源：四家医院_科室映射总表.json（26 个科室）
#pragma once

struct DeptEntry {
    const char* hospital_key;   // 中山 / 市六 / 仁济 / 肿瘤
    const char* hospital_name;  // 路线卡标题匹配用全名
    const char* name;           // 归一化口语科室名，与语音建议一致
    const char* lines[5];       // 大字页 5 行：楼栋 / 楼层 / 诊区 / 提示
    const char* hours;          // 门诊时间，走小字行
};

extern const DeptEntry kDepartments[];
extern const int kDepartmentCount;
