#include "ad_filter_engine.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QDir>
#include <QCoreApplication>

AdFilterEngine &AdFilterEngine::Instance() {
    static AdFilterEngine instance;
    return instance;
}

void AdFilterEngine::loadConfig(const QString &configPath) {
    QWriteLocker locker(&_lock);

    QString targetPath = configPath;
    if (targetPath.isEmpty()) {
        // 多路径自动探测，优先使用可执行文件同级目录
        const auto appDir = QCoreApplication::applicationDirPath();
        const QString candidate1 = appDir + "/ad_filter.json";
        const QString candidate2 = QDir::currentPath() + "/ad_filter.json";
        if (QFileInfo::exists(candidate1)) {
            targetPath = candidate1;
        } else if (QFileInfo::exists(candidate2)) {
            targetPath = candidate2;
        } else {
            targetPath = candidate1;
        }
    }
    _configPath = targetPath;

    QFileInfo fileInfo(targetPath);
    if (!fileInfo.exists()) {
        QJsonObject defaultObj;
        defaultObj["enabled"] = true;
        defaultObj["replace_with_placeholder"] = true;
        defaultObj["comment"] = QString::fromUtf8("本插件仅对频道生效；命中广告将替换为简短提示");

        QJsonArray defaultKws;
        defaultKws.append(QString::fromUtf8("代开发票"));
        defaultKws.append(QString::fromUtf8("兼职刷单"));
        defaultKws.append(QString::fromUtf8("博彩娱乐"));
        defaultKws.append(QString::fromUtf8("点击链接领取福利"));
        defaultKws.append(QString::fromUtf8("点击下方立即上车"));
        defaultKws.append(QString::fromUtf8("内幕代码"));
        defaultObj["keywords"] = defaultKws;

        QJsonArray defaultRegex;
        defaultRegex.append(QString::fromUtf8("t\\.me\\/\\+[a-zA-Z0-9_-]+"));
        defaultObj["regex"] = defaultRegex;

        defaultObj["whitelist_channels"] = QJsonArray();

        QFile initFile(targetPath);
        if (initFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            initFile.write(QJsonDocument(defaultObj).toJson(QJsonDocument::Indented));
            initFile.close();
        }
        _lastModifiedTime = QFileInfo(targetPath).lastModified().toMSecsSinceEpoch();
        _enabled = true;
        _replaceWithPlaceholder = true;
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

    QFile file(targetPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    const auto doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isObject()) return;

    const auto root = doc.object();
    _enabled = root.value("enabled").toBool(true);
    _replaceWithPlaceholder = root.value("replace_with_placeholder").toBool(true);

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

bool AdFilterEngine::shouldBlockMessage(
        bool isBroadcastChannel,
        const QString &text,
        uint64_t channelId,
        QString *outMatchedKeyword) const {
    if (!isBroadcastChannel || text.isEmpty()) {
        return false;
    }

    QReadLocker locker(&_lock);
    if (!_enabled) {
        return false;
    }

    // 白名单频道放行
    if (_whitelistChannelIds.contains(channelId)) {
        return false;
    }

    // 1. 普通关键词忽略大小写匹配
    for (const auto &kw : _plainKeywords) {
        if (!kw.isEmpty() && text.contains(kw, Qt::CaseInsensitive)) {
            if (outMatchedKeyword) {
                *outMatchedKeyword = kw;
            }
            return true;
        }
    }

    // 2. 正则表达式规则匹配
    for (const auto &regex : _regexList) {
        if (regex.isValid()) {
            const auto match = regex.match(text);
            if (match.hasMatch()) {
                if (outMatchedKeyword) {
                    *outMatchedKeyword = match.captured(0);
                }
                return true;
            }
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

    for (const auto &existing : _plainKeywords) {
        if (existing.compare(trimmed, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }

    _plainKeywords.append(trimmed);

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
    root["replace_with_placeholder"] = _replaceWithPlaceholder;

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

bool AdFilterEngine::replaceWithPlaceholder() const {
    QReadLocker locker(&_lock);
    return _replaceWithPlaceholder;
}

void AdFilterEngine::setReplaceWithPlaceholder(bool val) {
    QWriteLocker locker(&_lock);
    _replaceWithPlaceholder = val;
}

QStringList AdFilterEngine::getKeywords() const {
    QReadLocker locker(&_lock);
    return _plainKeywords;
}
