# 代码风格：注释规范

## 基本原则

1. 公共类和公共函数使用 Doxygen 文档注释。
2. 私有函数只在逻辑不明显时添加说明。
3. 函数内部重点解释设计原因、边界条件和特殊处理。
4. 避免逐行翻译代码。
5. 注释发生变化时，要与代码同步修改。
6. 已废弃代码交给 Git 保存，不长期注释在源码中。

## 注释风格

### 类和公共函数

```cpp
/**
 * @brief 管理热分析曲线
 *
 * 负责曲线的添加、删除、查找和激活状态管理。
 */
class CurveManager
{
public:
    /**
     * @brief 根据曲线 ID 查找曲线
     * @param curveId 曲线唯一标识
     * @return 找到时返回曲线指针，否则返回 nullptr
     */
    ThermalCurve* findCurve(const QString& curveId);
};
```

### 枚举和成员变量

```cpp
enum class InstrumentType
{
    TGA,  ///< 热重分析
    DSC,  ///< 差示扫描量热
    ARC   ///< 加速量热
};

QString m_activeCurveId;  ///< 当前激活曲线的唯一标识
```

### 简短接口

```cpp
/// 返回当前激活曲线。
ThermalCurve* activeCurve() const;
```

### 完整示例

```cpp
/**
 * @brief 执行移动平均滤波
 *
 * @param input 输入曲线
 * @param windowSize 平滑窗口大小
 * @return 处理后的曲线
 *
 * @pre windowSize 必须大于 0
 * @note 偶数窗口会自动调整为奇数
 * @warning 该函数可能复制大量曲线数据
 */
ThermalCurve smooth(
    const ThermalCurve& input,
    int windowSize);
```

## 常用 Doxygen 标签

| 标签 | 用途 |
|------|------|
| `@file` | 文件说明 |
| `@brief` | 一句话摘要 |
| `@param` | 参数说明 |
| `@return` | 返回值说明 |
| `@retval` | 特定返回值说明 |
| `@note` | 补充说明 |
| `@warning` | 警告信息 |
| `@see` | 交叉引用 |
| `@throws` | 可能抛出的异常 |
| `@pre` | 前置条件 |
| `@post` | 后置条件 |
| `@deprecated` | 已废弃标记 |
