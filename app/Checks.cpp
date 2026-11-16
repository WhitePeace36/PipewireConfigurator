#include "Checks.h"

#include "LiveState.h"
#include "Schema.h"
#include "SpaJson.h"

#include <QObject>

QList<int> parseIntList(const QString& text)
{
  QList<int> out;
  for (const QString& s : SpaJson::toStringList(text))
  {
    bool ok = false;
    const int v = s.toInt(&ok);
    if (ok && v > 0)
      out.append(v);
  }
  return out;
}

double fractionToMs(const QString& text)
{
  const QStringList parts = text.split('/');
  if (parts.size() != 2)
    return -1;
  bool ok1 = false, ok2 = false;
  const double num = parts[0].trimmed().toDouble(&ok1);
  const double den = parts[1].trimmed().toDouble(&ok2);
  if (!ok1 || !ok2 || den <= 0)
    return -1;
  return num / den * 1000.0;
}

QStringList runChecks(const QString& scope, const ValueGetter& value, const ValueGetter& graph)
{
  QStringList w;
  auto num = [](const QString& s)
  {
    bool ok = false;
    const long long v = s.toLongLong(&ok);
    return ok ? v : -1;
  };

  if (scope == Scope::Graph)
  {
    const long long rate = num(value("default.clock.rate"));
    const QList<int> allowed = parseIntList(value("default.clock.allowed-rates"));
    if (rate > 0 && !allowed.isEmpty() && !allowed.contains(rate))
      w.append(QObject::tr("The default rate (%1 Hz) is not one of the allowed rates.").arg(rate));

    const QStringList order = {"default.clock.quantum-floor", "default.clock.min-quantum", "default.clock.quantum", "default.clock.max-quantum", "default.clock.quantum-limit"};
    for (int i = 0; i + 1 < order.size(); ++i)
    {
      const long long a = num(value(order[i])), b = num(value(order[i + 1]));
      if (a > 0 && b > 0 && a > b)
      {
        const SettingDef* da = Schema::instance().find(order[i]);
        const SettingDef* db = Schema::instance().find(order[i + 1]);
        w.append(QObject::tr("%1 (%2) is larger than %3 (%4); PipeWire will clamp it.").arg(da->label).arg(a).arg(db->label.toLower()).arg(b));
      }
    }

    // PipeWire rounds quanta down to a power of two unless clock.power-of-two-quantum is off.
    if (value("clock.power-of-two-quantum") != "false")
    {
      // Only the resulting graph quantum is rounded, so check the values it can come from.
      QStringList rounded;
      for (const QString& key : {QStringLiteral("default.clock.min-quantum"), QStringLiteral("default.clock.quantum"), QStringLiteral("default.clock.max-quantum")})
      {
        const long long q = num(value(key));
        if (q > 0 && (q & (q - 1)) != 0)
        {
          long long p = 1;
          while (p * 2 <= q)
            p *= 2;
          rounded.append(QStringLiteral("%1 → %2").arg(q).arg(p));
        }
      }
      if (!rounded.isEmpty())
        w.append(QObject::tr("The graph quantum is rounded down to a power of two (%1). Turn off \"Round quantum to a power of two\" to use these values exactly.")
                   .arg(rounded.join(", ")));
    }

    // Default, minimum and maximum quantum scale with the rate (relative to the default rate), the
    // limit does not. At high rates the scaled maximum can hit the limit.
    const long long maxQ = num(value("default.clock.max-quantum"));
    const long long limit = num(value("default.clock.quantum-limit"));
    int highest = 0;
    for (int r : allowed)
      highest = qMax(highest, r);
    if (rate > 0 && maxQ > 0 && limit > 0 && highest > rate)
    {
      const long long scaled = maxQ * highest / rate;
      if (scaled > limit)
        w.append(QObject::tr("At %1 Hz the maximum quantum scales to %2, but the quantum limit (%3) caps it, so applications get less latency headroom than at %4 Hz. Raise the "
                             "quantum limit to at least %2.")
                   .arg(highest)
                   .arg(scaled)
                   .arg(limit)
                   .arg(rate));
    }
    if (num(value("default.clock.quantum-limit")) > 8192)
      w.append(QObject::tr("Many JACK applications have a built-in limit of 8192 samples and may fail with larger buffers."));
  }
  else if (scope == Scope::Stream)
  {
    const long long forced = num(value("node.force-rate"));
    const QList<int> allowed = parseIntList(graph("default.clock.allowed-rates"));
    if (forced > 0 && !allowed.isEmpty() && !allowed.contains(forced))
      w.append(QObject::tr("The forced rate (%1 Hz) is not one of the allowed rates on the Graph page.").arg(forced));
  }
  else if (scope == Scope::AlsaNode)
  {
    const long long rate = num(value("audio.rate"));
    const QList<int> allowed = parseIntList(value("audio.allowed-rates"));
    if (rate > 0 && !allowed.isEmpty() && !allowed.contains(rate))
      w.append(QObject::tr("The fixed rate (%1 Hz) is not one of this device's allowed rates.").arg(rate));
    const long long period = num(value("api.alsa.period-size"));
    const long long limit = num(graph("default.clock.quantum-limit"));
    if (period > 0 && limit > 0 && period > limit)
      w.append(QObject::tr("The period size is larger than the quantum limit (%1).").arg(limit));
  }
  else if (scope == Scope::BtGlobal)
  {
    const QStringList codecs = SpaJson::toStringList(value("bluez5.codecs"));
    if (!value("bluez5.codecs").isEmpty() && !codecs.contains("sbc"))
      w.append(QObject::tr("SBC is not allowed. It is the one codec every headphone supports, so without it some headphones will not connect in high-quality mode."));
    if (!value("bluez5.codecs").isEmpty() && !codecs.contains("msbc") && value("bluez5.enable-msbc") != "false")
      w.append(QObject::tr("mSBC is not in the allowed codecs, so headset microphones use the low-quality CVSD codec even with mSBC enabled. Tick mSBC "
                           "to keep wideband speech."));
    // Roles are the computer's side of the connection: it plays to headphones as the A2DP source.
    const QStringList roles = SpaJson::toStringList(value("bluez5.roles"));
    if (!value("bluez5.roles").isEmpty() && !roles.contains("a2dp_source"))
      w.append(QObject::tr("The A2DP source role is disabled, so headphones only get the low-quality headset profile."));
    const long long rate = num(value("bluez5.default.rate"));
    if (rate > 48000)
      w.append(QObject::tr("Only a few codecs (LDAC, LC3plus) support more than 48 kHz; others fall back to 48 or 44.1 kHz."));
  }
  return w;
}

QStringList deviceChecks(const LiveNode* node, const ValueGetter& value)
{
  QStringList w;
  const QString format = value("audio.format");
  const long long rate = value("audio.rate").toLongLong();
  const QList<int> allowed = parseIntList(value("audio.allowed-rates"));
  if (!node)
  {
    if (!format.isEmpty() || rate > 0)
      w.append(QObject::tr("The device is not running. If it disappeared after you set a sample format or a fixed rate, the device cannot "
                           "be opened with it: untick that setting and use Save and Apply."));
    return w;
  }

  // The device only reports its full abilities while nothing narrows them; otherwise there is nothing to compare.
  if (!format.isEmpty() && !node->supportedFormats.isEmpty() && !node->props.contains("audio.format") && !node->supportedFormats.contains(format))
  {
    QStringList usable;  // interleaved little-endian names, as offered on this page
    for (const QString& f : node->supportedFormats)
    {
      if (f.endsWith("LE"))
        usable.append(f);
    }
    w.append(QObject::tr("The device does not support %1 (it offers %2). PipeWire could not open it and the device would disappear.")
               .arg(format, usable.isEmpty() ? node->supportedFormats.join(", ") : usable.join(", ")));
  }
  const bool ratesKnown =
    (!node->supportedRates.isEmpty() || node->maxRate > 0) && !node->props.contains("audio.rate") && !node->props.contains("audio.allowed-rates");
  if (ratesKnown && rate > 0 && !node->supportsRate(int(rate)))
    w.append(QObject::tr("The device does not support %1 Hz. PipeWire could not open it and the device would disappear.").arg(rate));
  if (ratesKnown)
  {
    QStringList unsupported;
    for (int r : allowed)
    {
      if (!node->supportsRate(r))
        unsupported.append(QString::number(r));
    }
    if (!unsupported.isEmpty())
      w.append(QObject::tr("The device does not support %1 Hz, so it is not used at these rates.").arg(unsupported.join(", ")));
  }
  return w;
}
