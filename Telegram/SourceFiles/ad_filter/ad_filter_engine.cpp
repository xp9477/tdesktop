#include "ad_filter_engine.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QDir>

AdFilterEngine &AdFilterEngine::Instance() {
    static AdFilterEngine instance;
    return instance;
}

void AdFilterEngine::loadConfig(const QString &configPath) {
    QWriteLocker locker(&_lock);
    _configPath = configPath;

    QFileInfo fileInfo(configPath);
    if (!fileInfo.exists()) {
        // 若配置文件不存在，则自动初始化一份默认配置
        QJsonObject defaultObj;
        defaultObj["enabled"] = true;
        defaultObj["comment"] = QString::fromUtf8("本插件仅对广播频道(Broadcast Channel)生效，不影响群组与私聊");
        
        QJsonArray defaultKws;
        defaultKws.append(QString::fromUtf8("代开发票"));
        defaultKws.append(QString::fromUtf8("兼职刷单"));
        defaultKws.append(QString::fromUtf8("博彩娱乐"));
        defaultKws.append(QString::fromUtf8("点击链接领取福利"));
        defaultObj["keywords"] = defaultKws;

        QJsonArray defaultRegex;
        defaultRegex.append(QString::fromUtf8("t\\.me\\/\\+[a-zA-Z0-9_-]+"));
        defaultObj["regex"] = defaultRegex;

        defaultObj["whitelist_channels"] = QJsonArray();

        QFile initFile(configPath);
        if (initFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            initFile.write(QJsonDocument(defaultObj).toJson(QJsonDocument::Indented));
            initFile.close();
        }
        _lastModifiedTime = QFileInfo(configPath).lastModified().toMSecsSinceEpoch();
        _enabled = true;
        _plainKeywords.clear();
        for (const auto &v : defaultKws) _plainKeywords.append(v.toString());
        _regexList.clear();
        for (const auto &v : defaultRegex) {
            _regexList.append(QRegularExpression(v.toString(), QRegularExpression::CaseInsensitiveOption));
        }
        _whitelistChannelIds.clear();
        return;
    }

    _lastModifiedTime = fileInfo.lastModified().toMSecsSinceEpoch();

    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    const auto doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isObject()) return;

    const auto root = doc.object();
    _enabled = root.value("enabled").toBool(true);

    _plainKeywords.clear();
    const auto kwArray = root.value("keywords").toArray();
    for (const auto &val : kwArray) {
        const auto kw = val.toString().trimmed();
        if (!kw.isEmpty()) {
            _plainKeywords.append(kw);
        }
    }

    _regexList.clear();
    const auto regArray = root.value("regex").toArray();
    for (const auto &val : regArray) {
        const auto regStr = val.toString().trimmed();
        if (!regStr.isEmpty()) {
            _regexList.append(QRegularExpression(regStr, QRegularExpression::CaseInsensitiveOption));
        }
    }

    _whitelistChannelIds.clear();
    const auto wlArray = root.value("whitelist_channels").toArray();
    for (const auto &val : wlArray) {
        _whitelistChannelIds.insert(static_cast<uint64_t>(val.toVariant().toULongLong()));
    }
}

void AdFilterEngine::reloadIfNeeded() {
    if (_configPath.isEmpty()) return;

    QFileInfo fileInfo(_configPath);
    if (fileInfo.exists() && fileInfo.lastModified().toMSecsSinceEpoch() > _lastModifiedTime) {
        loadConfig(_configPath);
    }
}

bool AdFilterEngine::shouldBlockMessage(bool isBroadcastChannel, const QString &text, uint64_t channelId) const {
    // 快速短路：非频道（群组、超级群、私聊）或空内容绝对不走匹配，保证群聊吞吐零损耗
    if (!isBroadcastChannel || text.isEmpty()) {
        return false;
    }

    QReadLocker locker(&_lock);
    if (!_enabled) {
        return false;
    }

    // 白名单放行
    if (_whitelistChannelIds.contains(channelId)) {
        return false;
    }

    // 1. 普通关键词忽略大小写匹配
    for (const auto &kw : _plainKeywords) {
        if (!kw.isEmpty() && text.contains(kw, Qt::CaseInsensitive)) {
            return true;
        }
    }

    // 2. 正则表达式规则匹配
    for (const auto &regex : _regexList) {
        if (regex.isValid() && regex.match(text).hasMatch()) {
            return true;
        }
    }

    return false;
}

bool AdFilterEngine::addKeywordAndSave(const QString &keyword) {
    const auto trimmed = keyword.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }

    QWriteLocker locker(&_lock);

    // 避免重复追加
    for (const auto &existing : _plainKeywords) {
        if (existing.compare(trimmed, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }

    _plainKeywords.append(trimmed);

    // 尝试写回 JSON 文件
    QFile file(_configPath);
    QJsonObject root;
    if (file.open(QIODevice::ReadOnly)) {
        const auto doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) {
            root = doc.object();
        }
        file.close();
    }

    QJsonArray kwArray;
    for (const auto &kw : _plainKeywords) {
        kwArray.append(kw);
    }
    root["keywords"] = kwArray;
    root["enabled"] = _enabled;

    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.close();
    }

    QFileInfo fi(_configPath);
    _lastModifiedTime = fi.lastModified().toMSecsSinceEpoch();
    return true;
}

bool AdFilterEngine::removeKeywordAndSave(const QString &keyword) {
    const auto trimmed = keyword.trimmed();
    if (trimmed.isEmpty()) return false;

    QWriteLocker locker(&_lock);
    bool removed = false;
    for (int i = _plainKeywords.size() - 1; i >= 0; --i) {
        if (_plainKeywords[i].compare(trimmed, Qt::CaseInsensitive) == 0) {
            _plainKeywords.removeAt(i);
            removed = true;
        }
    }

    if (!removed) return false;

    QFile file(_configPath);
    QJsonObject root;
    if (file.open(QIODevice::ReadOnly)) {
        const auto doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) root = doc.object();
        file.close();
    }

    QJsonArray kwArray;
    for (const auto &kw : _plainKeywords) kwArray.append(kw);
    root["keywords"] = kwArray;

    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.close();
    }

    QFileInfo fi(_configPath);
    _lastModifiedTime = fi.lastModified().toMSecsSinceEpoch();
    return true;
}

void AdFilterEngine::addWhitelistChannel(uint64_t channelId) {
    QWriteLocker locker(&_lock);
    _whitelistChannelIds.insert(channelId);
}

bool AdFilterEngine::isEnabled() const {
    QReadLocker locker(&_lock);
    return _enabled;
}

void AdFilterEngine::setEnabled(bool enabled) {
    QWriteLocker locker(&_lock);
    _enabled = enabled;
}

QStringList AdFilterEngine::getKeywords() const {
    QReadLocker locker(&_lock);
    return _plainKeywords;
}
