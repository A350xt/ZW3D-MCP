#include "errors.h"
#include "VxApi.h"

namespace {
struct Info { const char* code; const char* message; const char* hint; };

Info lookup(int e) {
  switch (e) {
    case ZW_API_GENERAL_ERROR:      return {"GENERAL_ERROR", "中望3D内部函数失败", "检查模型状态后重试；可调用 list_shapes 确认当前实体。"};
    case ZW_API_ROOT_OBJ_ACT_FAIL:  return {"ROOT_ACT_FAIL", "无法激活当前根对象", "先调用 new_part 新建零件，或在中望3D中打开一个零件。"};
    case ZW_API_WRONG_ROOT_ENV:     return {"WRONG_ROOT_ENV", "当前不在零件建模环境", "先调用 new_part 或在中望3D中打开一个零件。"};
    case ZW_API_ROOT_SINGLE_MULTI_TYPE_ERROR: return {"ROOT_FILE_TYPE", "单根/多根文件类型不匹配", "改用 new_part 新建标准零件文件。"};
    case ZW_API_ROOT_ERROR:         return {"ROOT_NOT_FOUND", "找不到指定的根对象", "检查零件名称。"};
    case ZW_API_INVALID_INPUT:      return {"INVALID_INPUT", "输入参数无效（空指针、id<1 或数值超出范围）", "检查尺寸是否为正、id 是否来自 list_shapes/list_edges。"};
    case ZW_API_INPUT_ERROR:        return {"INPUT_ERROR", "为命令准备输入数据时出错", "检查轮廓是否闭合、共面，参数是否合理。"};
    case ZW_API_INPUT_TYPE_ERROR:   return {"INPUT_TYPE_ERROR", "输入对象类型不符合要求", "确认传入的 id 是实体/边/面中正确的一种。"};
    case ZW_API_INVALID_OUTPUT:     return {"INVALID_OUTPUT", "输出参数无效", "这是插件内部错误，请报告。"};
    case ZW_API_FILE_LOCATE_FAIL:   return {"FILE_NOT_FOUND", "找不到指定文件", "使用完整路径。"};
    case ZW_API_FILE_ERROR:         return {"FILE_ERROR", "指定文件不在内存中", "先打开或新建文件。"};
    case ZW_API_OBJ_TYPE_ERROR:     return {"OBJ_TYPE_ERROR", "对象类型错误", "确认 id 的类型。"};
    case ZW_API_OBJ_DATA_GET_ERROR: return {"OBJ_DATA_GET_ERROR", "读取对象数据失败", "该 id 可能已不存在（被布尔运算消耗），调用 list_shapes 刷新。"};
    case ZW_API_OBJ_DATA_SET_ERROR: return {"OBJ_DATA_SET_ERROR", "写入对象数据失败", ""};
    case ZW_API_INVALID_OBJ:        return {"INVALID_OBJ", "对象不存在或未激活", "调用 list_shapes 查看当前有效 id。"};
    case ZW_API_UNSUPPORTED_OBJ:    return {"UNSUPPORTED_OBJ", "该对象不支持此操作", ""};
    case ZW_API_GET_NEW_OBJ_FAIL:   return {"NEW_OBJ_FAIL", "未能取得新生成对象的 id", "调用 list_shapes 查看结果。"};
    case ZW_API_CMMD_INIT_ERROR:    return {"CMMD_INIT_ERROR", "命令初始化失败", "中望3D可能正在执行其他命令或弹出对话框；按 Esc 退出后重试。"};
    case ZW_API_CMMD_EXEC_ERROR:    return {"CMMD_EXEC_ERROR", "命令执行失败", "几何条件不满足：轮廓自相交/不闭合、半径过大、实体不相交等；调整参数后重试。"};
    case ZW_API_CMMD_DATA_ERROR:    return {"CMMD_DATA_ERROR", "命令数据容器错误", ""};
    case ZW_API_FUN_REGISTER_ERROR: return {"FUN_REGISTER_ERROR", "函数已注册", "插件可能已加载过。"};
    case ZW_API_MEMORY_ERROR:       return {"MEMORY_ERROR", "内存分配失败", ""};
    case ZW_API_OUT_OF_RANGE:       return {"OUT_OF_RANGE", "超出范围（名称/路径过长）", "缩短文件名或路径（上限约 511 字节）。"};
    case ZW_API_MODELING_DATA_ERROR:return {"MODELING_DATA_ERROR", "几何建模数据错误", "操作产生了非法几何；检查尺寸、位置是否合理。"};
    case ZW_API_BOOLEAN_ADD_BASE_ERROR: return {"BOOLEAN_ADD_BASE_ERROR", "布尔加失败：工具体与多个基体相交", "分步合并，或改用 boolean 工具指定 base_id。"};
    case ZW_API_BOOLEAN_CUT_BASE_ERROR: return {"BOOLEAN_CUT_BASE_ERROR", "布尔减失败：没有基体与工具体相交", "检查工具体位置是否与基体重叠。"};
    case ZW_API_INVALID_NAME:       return {"INVALID_NAME", "名称无效（为空或含 \\ / : * ? \" < > |）", "更换名称。"};
    case ZW_API_INVALID_PATH:       return {"INVALID_PATH", "文件路径无效", "使用完整的绝对路径，并确认目录存在。"};
    case ZW_API_INVALID_STRING:     return {"INVALID_STRING", "字符串为空", ""};
    case ZW_API_PATH_LEN_OVER_RANGE:return {"PATH_TOO_LONG", "路径过长", "缩短路径。"};
    case ZW_API_NAME_LEN_OVER_RANGE:return {"NAME_TOO_LONG", "名称过长", "缩短名称。"};
    case ZW_API_NAME_ALREADY_IN_USE:return {"NAME_IN_USE", "名称已被占用", "换一个名称。"};
    default:                        return {"API_ERROR", "中望3D API 返回错误", ""};
  }
}
}  // namespace

void check_api(int err, const char* what) {
  if (err == 0) return;
  Info i = lookup(err);
  throw ApiError(i.code, std::string(what) + ": " + i.message, i.hint, err);
}

[[noreturn]] void invalid_input(const std::string& message, const std::string& hint) {
  throw ApiError("INVALID_INPUT", message, hint, ZW_API_INVALID_INPUT);
}
