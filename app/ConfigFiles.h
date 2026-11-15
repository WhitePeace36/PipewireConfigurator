#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>

// Locations of the config files, and the merged view of the configuration PipeWire/WirePlumber load
// from everything except this tool's own files.

enum class ConfigFile
{
  Pipewire,     // pipewire.conf
  Client,       // client.conf
  ClientRt,     // client-rt.conf, used by realtime clients before PipeWire 1.4
  Pulse,        // pipewire-pulse.conf
  Jack,         // jack.conf, read by JACK applications through pipewire-jack
  WirePlumber,  // wireplumber.conf
};

QString configFileName(ConfigFile f);
QList<ConfigFile> allConfigFiles();

// A name that does not exist yet for a backup of |path|: <path>.bak-<date>[-<n>].
QString backupPath(const QString& path);

struct ConfigPaths
{
  QString userConfigHome;          // $XDG_CONFIG_HOME or ~/.config
  QStringList systemPipewireDirs;  // in load order, lowest priority first
  QStringList systemWireplumberDirs;  // same; includes $XDG_CONFIG_DIRS and $XDG_DATA_DIRS like WirePlumber

  static ConfigPaths fromEnvironment();
  static QString ownFileName() { return QStringLiteral("60-pipewire-configurator.conf"); }
  // Name of our drop-in up to version 2.0.x (Pipewire Simple Configurator).
  static QString oldOwnFileName() { return QStringLiteral("60-simple-configurator.conf"); }

  QString userDir(ConfigFile f) const;  // ~/.config/pipewire or ~/.config/wireplumber
  QStringList systemDirs(ConfigFile f) const;
  QString ownFile(ConfigFile f) const;         // our drop-in
  QString oldOwnFile(ConfigFile f) const;      // our drop-in under its name up to 2.0.x
  // Renames drop-ins saved under the old name to ownFileName(), so settings made with 2.0.x stay ours.
  // When both names exist the new file wins and the old one becomes a backup (*.bak-<date>, which
  // PipeWire doesn't load). Returns one message per file that could not be moved.
  QStringList migrateOldOwnFiles() const;
  QString legacyUserCopy(ConfigFile f) const;  // full copy in the user dir, e.g. ~/.config/pipewire/pipewire.conf
  // PipeWire before 1.4 has a separate client-rt.conf that realtime clients (pw-cat, the ALSA plugin, ...)
  // read instead of client.conf; stream settings must then be written to both.
  bool hasClientRt() const;
};

struct ValueOrigin
{
  QVariant value;
  QString file;
  bool isValid() const { return !file.isEmpty(); }
};

// Merged configuration of one config file family (main file plus drop-ins), excluding our own drop-in.
// For WirePlumber, a section written as "override.<section>" replaces the section instead of merging
// into it (PipeWire has no such prefix).
class EffectiveConfig
{
public:
  static EffectiveConfig load(const ConfigPaths& paths, ConfigFile which);

  // Value set for |key| in object section |section| by any file except ours.
  ValueOrigin value(const QString& section, const QString& key) const;
  // Value set by a drop-in that is loaded after ours and therefore overrides our setting. When that
  // drop-in replaces the whole section without setting |key|, the result has a file but no value.
  ValueOrigin overriddenBy(const QString& section, const QString& key) const;

  QString mainFile;
  QStringList dropInsBefore;  // loaded before our file
  QStringList dropInsAfter;   // loaded after our file (they win)
  QStringList errors;

private:
  QMap<QString, QMap<QString, ValueOrigin>> m_before;
  QMap<QString, QMap<QString, ValueOrigin>> m_after;
  QMap<QString, QString> m_replacedAfter;  // section -> later drop-in that replaces it ("override.")
};
