// 由 tools/gen_dept_table.py 生成，不要手改。
// 来源：四家医院_科室映射总表.json → 复旦大学附属中山医院（9 个科室）
#pragma once

struct DeptEntry {
    const char* name;      // 归一化口语科室名，与语音建议一致
    const char* lines[5];  // 大字页 5 行：楼栋 / 诊室 / 提示
    const char* hours;     // 门诊时间，走小字行
};

extern const DeptEntry kDepartments[];
extern const int kDepartmentCount;
