#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QSet>
#include <QRegularExpression>
#include <QReadWriteLock>
#include <cstdint>

class AdFilterEngine final {
public:
    static AdFilterEngine &Instance();

    // 加载或初始化配置文件路径
    void loadConfig(const QString &configPath);

    // 检查文件修改时间并在需要时热重载
    void reloadIfNeeded();

    // 核心拦截判定：只针对广播频道生效
    bool shouldBlockMessage(bool isBroadcastChannel, const QString &text, uint64_t channelId) const;

    // 动态添加关键词并持久化写回配置文件
    bool addKeywordAndSave(const QString &keyword);

    // 动态移除关键词
    bool removeKeywordAndSave(const QString &keyword);

    // 白名单频道管理
    void addWhitelistChannel(uint64_t channelId);

    // 开关状态控制
    bool isEnabled() const;
    void setEnabled(bool enabled);

    // 获取当前过滤词列表快照
    QStringList getKeywords() const;

private:
    AdFilterEngine() = default;
    ~AdFilterEngine() = default;
    AdFilterEngine(const AdFilterEngine &) = delete;
    AdFilterEngine &operator=(const AdFilterEngine &) = delete;

    mutable QReadWriteLock _lock;
    QString _configPath;
    qint64 _lastModifiedTime = 0;

    bool _enabled = true;
    QStringList _plainKeywords;
    QVector<QRegularExpression> _regexList;
    QSet<uint64_t> _whitelistChannelIds;
};
