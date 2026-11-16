#include "LiveState.h"

#include <algorithm>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>

namespace
{
QString jsonToString(const QJsonValue& v)
{
  switch (v.type())
  {
    case QJsonValue::Bool:
      return v.toBool() ? "true" : "false";
    case QJsonValue::Double:
    {
      const double d = v.toDouble();
      if (d == static_cast<qint64>(d))
        return QString::number(static_cast<qint64>(d));
      return QString::number(d, 'g', 10);
    }
    case QJsonValue::String:
      return v.toString();
    case QJsonValue::Array:
    {
      QStringList parts;
      for (const QJsonValue& e : v.toArray())
        parts.append(jsonToString(e));
      return "[ " + parts.join(' ') + " ]";
    }
    default:
      return {};
  }
}

LiveNode::Kind kindOf(const QString& mediaClass, const QString& api)
{
  const bool bt = api == "bluez5";
  if (mediaClass == "Audio/Sink")
    return bt ? LiveNode::BtOutput : (api == "alsa" ? LiveNode::AlsaOutput : LiveNode::Other);
  if (mediaClass == "Audio/Source")
    return bt ? LiveNode::BtInput : (api == "alsa" ? LiveNode::AlsaInput : LiveNode::Other);
  return LiveNode::Other;
}
}  // namespace

bool LiveNode::supportsRate(int r) const
{
  if (supportedRates.contains(r))
    return true;
  return maxRate > 0 && r >= minRate && r <= maxRate;
}

QString LiveNode::currentValue(const QString& key) const
{
  // The negotiated format is not a property, but it is what these settings control.
  if (key == "audio.format" && !format.isEmpty())
    return format;
  if (key == "audio.rate" && rate > 0)
    return QString::number(rate);
  if (adapterValues.contains(key))
    return adapterValues.value(key).toString();
  if (props.contains(key))
    return props.value(key).toString();
  return {};
}

QString LiveNode::formatSummary() const
{
  QStringList parts;
  if (!codec.isEmpty())
    parts.append(codec.toUpper());
  if (!format.isEmpty())
    parts.append(format);
  if (rate > 0)
    parts.append(QStringLiteral("%1 Hz").arg(rate));
  if (channels > 0)
    parts.append(channels == 1 ? QObject::tr("mono") : channels == 2 ? QObject::tr("stereo") : QObject::tr("%1 channels").arg(channels));
  return parts.join(QStringLiteral(" · "));
}

LiveSnapshot LiveSnapshot::parse(const QByteArray& pwDumpJson)
{
  LiveSnapshot s;
  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(pwDumpJson, &err);
  if (!doc.isArray())
  {
    s.error = QObject::tr("Could not read pw-dump output: %1").arg(err.errorString());
    return s;
  }

  QMap<int, QJsonObject> clientProps;  // client id -> properties
  for (const QJsonValue& ov : doc.array())
  {
    const QJsonObject o = ov.toObject();
    const QString type = o.value("type").toString().section(':', -1);
    const QJsonObject info = o.value("info").toObject();
    const QJsonObject props = info.value("props").toObject();

    if (type == "Core")
    {
      s.pipewireVersion = info.value("version").toString();
    }
    else if (type == "Metadata")
    {
      const QString metaName = o.value("props").toObject().value("metadata.name").toString();
      for (const QJsonValue& mv : o.value("metadata").toArray())
      {
        const QJsonObject m = mv.toObject();
        const QString key = m.value("key").toString();
        const QJsonValue value = m.value("value");
        if (metaName == "default")
        {
          const QString name = value.isObject() ? value.toObject().value("name").toString() : value.toString();
          if (key == "default.audio.sink")
            s.defaultSink = name;
          else if (key == "default.audio.source")
            s.defaultSource = name;
        }
        else if (metaName == "settings")
        {
          s.settings.insert(key, jsonToString(value));
        }
      }
    }
    else if (type == "Node")
    {
      LiveNode n;
      n.id = o.value("id").toInt();
      n.name = props.value("node.name").toString();
      n.description = props.value("node.description").toString();
      if (n.description.isEmpty())
        n.description = props.value("node.nick").toString();
      n.mediaClass = props.value("media.class").toString();
      n.state = info.value("state").toString();
      n.btAddress = props.value("api.bluez5.address").toString();
      n.codec = props.value("api.bluez5.codec").toString();
      n.kind = kindOf(n.mediaClass, props.value("device.api").toString());
      for (auto it = props.begin(); it != props.end(); ++it)
        n.props.insert(it.key(), jsonToString(it.value()));

      const QJsonObject params = info.value("params").toObject();
      const QJsonArray formats = params.value("Format").toArray();
      if (!formats.isEmpty())
      {
        const QJsonObject f = formats.first().toObject();
        n.format = f.value("format").toString();
        n.rate = f.value("rate").toInt();
        n.channels = f.value("channels").toInt();
      }
      // EnumFormat: "format" and "rate" are a value, a choice {default, alt1, ...} or a range {default, min, max}.
      // Some PipeWire versions report the converter's side here (F32, rates 1..2^31) instead of the hardware's;
      // a range no sound card has means the entry says nothing about the device.
      for (const QJsonValue& ev : params.value("EnumFormat").toArray())
      {
        const QJsonObject e = ev.toObject();
        const QJsonObject range = e.value("rate").toObject();
        if (range.contains("max") && (range.value("max").toInt() > 1536000 || range.value("min").toInt() < 1000))
          continue;
        const QJsonValue fmt = e.value("format");
        if (fmt.isString())
          n.supportedFormats.append(fmt.toString());
        const QJsonObject fmtChoice = fmt.toObject();
        for (auto it = fmtChoice.begin(); it != fmtChoice.end(); ++it)
          n.supportedFormats.append(it.value().toString());
        const QJsonValue rate = e.value("rate");
        const QJsonObject r = rate.toObject();
        if (rate.isDouble())
          n.supportedRates.append(rate.toInt());
        else if (r.contains("min") && r.contains("max"))
        {
          n.minRate = n.minRate ? qMin(n.minRate, r.value("min").toInt()) : r.value("min").toInt();
          n.maxRate = qMax(n.maxRate, r.value("max").toInt());
        }
        else
        {
          for (auto it = r.begin(); it != r.end(); ++it)
            n.supportedRates.append(it.value().toInt());
        }
      }
      n.supportedFormats.removeDuplicates();
      n.supportedFormats.removeAll(QString());
      for (const QJsonValue& pv : params.value("Props").toArray())
      {
        const QJsonArray list = pv.toObject().value("params").toArray();
        for (int i = 0; i + 1 < list.size(); i += 2)
          n.adapterValues.insert(list.at(i).toString(), jsonToString(list.at(i + 1)));
      }
      s.nodes.append(n);
    }
    else if (type == "Device")
    {
      LiveDevice d;
      d.id = o.value("id").toInt();
      d.name = props.value("device.name").toString();
      d.description = props.value("device.description").toString();
      d.api = props.value("device.api").toString();
      d.btAddress = props.value("api.bluez5.address").toString();
      const QJsonArray profiles = info.value("params").toObject().value("Profile").toArray();
      if (!profiles.isEmpty())
        d.profile = profiles.first().toObject().value("description").toString();
      s.devices.append(d);
    }
    else if (type == "Client")
    {
      clientProps.insert(o.value("id").toInt(), props);
      if (props.value("client.api").toString() != "pipewire-pulse")
        continue;
      LiveClient c;
      c.binary = props.value("application.process.binary").toString();
      c.appName = props.value("application.name").toString();
      if (!c.binary.isEmpty())
        s.pulseClients.append(c);
    }
  }

  // Native PipeWire programs with an audio stream. Their stream.rules can only match what the stream itself
  // carries: application.name, or node.name for libraries that set no application.name (e.g. GStreamer).
  for (const LiveNode& n : s.nodes)
  {
    if (!n.mediaClass.startsWith("Stream/") || !n.mediaClass.endsWith("/Audio"))
      continue;
    const QJsonObject client = clientProps.value(n.props.value("client.id").toInt());
    if (client.value("client.api").toString() == "pipewire-pulse")
      continue;  // PulseAudio programs have their own rules
    LiveClient c;
    c.appName = n.props.value("application.name").toString();
    if (c.appName.isEmpty())
      c.appName = n.name;
    c.binary = client.value("application.process.binary").toString();
    if (c.appName.isEmpty())
      continue;
    const bool known = std::any_of(s.nativeClients.begin(), s.nativeClients.end(), [&](const LiveClient& o) { return o.appName == c.appName; });
    if (!known)
      s.nativeClients.append(c);
  }
  s.valid = true;
  return s;
}

const LiveNode* LiveSnapshot::node(const QString& name) const
{
  for (const LiveNode& n : nodes)
  {
    if (n.name == name)
      return &n;
  }
  return nullptr;
}

QList<const LiveNode*> LiveSnapshot::nodesOf(LiveNode::Kind kind) const
{
  QList<const LiveNode*> out;
  for (const LiveNode& n : nodes)
  {
    if (n.kind == kind)
      out.append(&n);
  }
  return out;
}

const LiveDevice* LiveSnapshot::btDevice(const QString& address) const
{
  for (const LiveDevice& d : devices)
  {
    if (d.api == "bluez5" && d.btAddress.compare(address, Qt::CaseInsensitive) == 0)
      return &d;
  }
  return nullptr;
}

const LiveNode* LiveSnapshot::btNode(const QString& address, bool output) const
{
  for (const LiveNode& n : nodes)
  {
    if (n.kind == (output ? LiveNode::BtOutput : LiveNode::BtInput) && n.btAddress.compare(address, Qt::CaseInsensitive) == 0)
      return &n;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------------------------

LiveState::LiveState(QObject* parent) : QObject(parent) {}

void LiveState::refresh()
{
  if (!m_dumpFile.isEmpty())
  {
    QFile f(m_dumpFile);
    if (!f.open(QIODevice::ReadOnly))
      finish({}, tr("Cannot read %1").arg(m_dumpFile));
    else
      finish(f.readAll(), {});
    return;
  }
  if (m_process)
    return;  // already running

  m_process = new QProcess(this);
  QProcess* proc = m_process;
  connect(proc, &QProcess::finished, this,
          [this, proc](int code, QProcess::ExitStatus status)
          {
            const QByteArray out = proc->readAllStandardOutput();
            const QString err = QString::fromLocal8Bit(proc->readAllStandardError()).trimmed();
            m_process = nullptr;
            proc->deleteLater();
            if (status != QProcess::NormalExit || code != 0)
              finish({}, err.isEmpty() ? tr("pw-dump failed (is PipeWire running?)") : err);
            else
              finish(out, {});
          });
  connect(proc, &QProcess::errorOccurred, this,
          [this, proc](QProcess::ProcessError e)
          {
            if (e != QProcess::FailedToStart)
              return;
            m_process = nullptr;
            proc->deleteLater();
            finish({}, tr("pw-dump was not found. Install the PipeWire tools (pipewire-bin or pipewire-utils) to see your devices."));
          });
  QTimer::singleShot(8000, proc, [proc] { proc->kill(); });
  proc->start("pw-dump", {});
}

void LiveState::finish(const QByteArray& data, const QString& error)
{
  if (!error.isEmpty())
  {
    if (m_snapshot.error == error)
      return;
    m_lastData.clear();
    m_snapshot = LiveSnapshot();
    m_snapshot.error = error;
  }
  else
  {
    if (data == m_lastData && m_snapshot.valid)
      return;  // the periodic refresh found nothing new
    m_lastData = data;
    m_snapshot = LiveSnapshot::parse(data);
  }
  emit updated();
}
