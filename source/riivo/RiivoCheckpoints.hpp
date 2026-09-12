/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Validation-boundary checkpoint order, template over caller-provided
 * light/log sinks. Production passes the real register/card sinks;
 * host tests pass recording mocks to prove the order: explicit ON,
 * entry line, validate, explicit OFF - with no toggle anywhere in the
 * span, so ordinary progress logging cannot cancel the reported states.
 * The sink interfaces deliberately have no Pulse method: code written
 * against them cannot express a toggle at all.
 *
 * What this cannot prove: physical LED visibility (brightness, timing
 * perception, whether anyone watches). An unobserved signal stays
 * inconclusive no matter what the order guarantees.
 *
 * Pure: no console calls, so host tests exercise this exact template.
 ***************************************************************************/
#ifndef RIIVO_CHECKPOINTS_HPP_
#define RIIVO_CHECKPOINTS_HPP_

namespace Riivo
{

template <typename Light, typename Log, typename Validate>
inline void ValidationBoundary(Light &light, Log &log, Validate validate,
							   const char *entryText)
{
	light.Set(true);
	log.Line(entryText);
	validate();
	light.Set(false);
}

} // namespace Riivo

#endif
