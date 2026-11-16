#pragma once

#include "ConfigFiles.h"

#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVariant>

// key -> value. Scalars are QString, lists are QStringList. Values the configurator does not manage
// (hand edits) may also be a QVariantMap or a QVariantList of objects; they are written back as read.
using Values = QMap<QString, QVariant>;

// One "matches = [ { key = value } ] actions = { update-props = { ... } }" rule.
// For application rules the special key "quirks" is written to actions.quirks instead.
struct Rule
{
  QString matchKey;
  QString matchValue;
  Values props;

  bool operator==(const Rule& o) const { return matchKey == o.matchKey && matchValue == o.matchValue && props == o.props; }
};

// Everything this tool writes. It lives entirely in four drop-in files that the tool owns, so the
// files themselves are the saved state and nothing else in the user's configuration is touched.
class ConfigModel
{
public:
  Values graph;                  // pipewire.conf.d          context.properties
  Values stream;                 // client.conf.d + pipewire-pulse.conf.d   stream.properties
  Values pulse;                  // pipewire-pulse.conf.d    pulse.properties
  Values bluez;                  // wireplumber.conf.d       monitor.bluez.properties
  Values wpSettings;             // wireplumber.conf.d       wireplumber.settings
  QList<Rule> alsaRules;         // monitor.alsa.rules  (node.name)
  QList<Rule> bluezRules;        // monitor.bluez.rules (device.name / node.name)
  QList<Rule> appRules;          // pulse.rules + stream.rules in pipewire-pulse.conf.d (application.process.binary)
  // Native PipeWire programs: stream.rules in client.conf.d. matchKey is "application.name"; the rule also
  // matches node.name, because some libraries (e.g. GStreamer) put the program name only there.
  QList<Rule> nativeAppRules;
  bool includeClientRt = false;  // also write stream settings to client-rt.conf.d (PipeWire < 1.4)

  // Taken from the rest of the configuration by load(), not saved state: context.properties.rules of PipeWire's own config (e.g. min-quantum 1024 on virtual machines). Rules are
  // applied after context.properties, so the graph keys they set are repeated in a rule with the same matches.
  QVariantList graphRulesElsewhere;

  // Reads the tool's own drop-ins. Problems (e.g. hand edits that do not parse) go to |warnings|, and
  // the files that could not be parsed (and are therefore ignored) to |unreadable|. Files written by an
  // older version in a layout that no longer works as intended go to |outdated|; they should be rewritten.
  static ConfigModel load(const ConfigPaths& paths, QStringList* warnings = nullptr, QSet<ConfigFile>* unreadable = nullptr, QSet<ConfigFile>* outdated = nullptr);

  // Graph settings that clients read from their own context.properties as well. Clients size some
  // buffers from their own config (e.g. pw_filter, port defaults), so a raised limit must reach them too.
  static const QStringList& clientContextKeys();
  // Stream settings that the PulseAudio bridge always sets itself, so writing them there has no effect.
  static const QStringList& pulseIgnoredStreamKeys();
  // WirePlumber appends arrays from later files to earlier ones; these keys are written as
  // "override.<key>" so the list replaces what other files allow.
  static const QStringList& wpReplacedListKeys();

  // File contents for every drop-in; an empty string means the file should not exist.
  QMap<ConfigFile, QString> render() const;

  bool operator==(const ConfigModel& o) const;
  bool operator!=(const ConfigModel& o) const { return !(*this == o); }

  // Finds the rule matching key = value, optionally creating it.
  static Rule* findRule(QList<Rule>& rules, const QString& key, const QString& value, bool create);
  static const Rule* findRule(const QList<Rule>& rules, const QString& key, const QString& value);
  void removeEmptyRules();

  // Rule match values used for devices.
  // |address| may be written with ':' (as BlueZ reports it) or '_' (as used in node names).
  static QString btId(const QString& address) { return QString(address).replace(':', '_').toUpper(); }
  static QString btDeviceName(const QString& address) { return "bluez_card." + btId(address); }
  static QString btOutputPattern(const QString& address) { return "~bluez_output." + btId(address) + ".*"; }
  static QString btInputPattern(const QString& address) { return "~bluez_input." + btId(address) + ".*"; }
};

// Imports managed keys from full copies of the main config files that older versions of this tool
// (or the user) placed in ~/.config/pipewire. Only keys known to the schema and differing from the
// system configuration are taken over.
struct LegacyImport
{
  QStringList files;     // legacy copies found
  QStringList imported;  // "file: key = value" descriptions
};
LegacyImport findLegacyCopies(const ConfigPaths& paths);
LegacyImport importLegacyCopies(const ConfigPaths& paths, ConfigModel& model);

// Renames the legacy copies to *.bak-<date>, all or nothing: when one cannot be renamed, the ones
// renamed so far are moved back. |backups| receives the new names, |error| a message on failure.
bool retireLegacyCopies(const QStringList& files, QStringList* backups, QString* error);
