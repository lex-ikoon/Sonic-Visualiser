/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Sonic Visualiser
    An audio file viewer and annotation editor.
    Centre for Digital Music, Queen Mary, University of London.
    This file copyright 2006-2009 Chris Cannam and QMUL.
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#include "SpectrogramLayer.h"

#include "view/View.h"
#include "base/Profiler.h"
#include "base/AudioLevel.h"
#include "base/Window.h"
#include "base/Pitch.h"
#include "base/Preferences.h"
#include "base/RangeMapper.h"
#include "base/LogRange.h"
#include "widgets/CommandHistory.h"
#include "ColourMapper.h"
#include "ImageRegionFinder.h"
#include "data/model/Dense3DModelPeakCache.h"

#include <QPainter>
#include <QImage>
#include <QPixmap>
#include <QRect>
#include <QTimer>
#include <QApplication>
#include <QMessageBox>
#include <QMouseEvent>
#include <QTextStream>

#include <iostream>

using std::cerr;
using std::endl;
#include <cassert>
#include <cmath>

#ifndef __GNUC__
#include <alloca.h>
#endif

//#define DEBUG_SPECTROGRAM_REPAINT 1

SpectrogramLayer::SpectrogramLayer(Configuration config) :
    m_model(0),
    m_channel(0),
    m_windowSize(1024),
    m_windowType(HanningWindow),
    m_windowHopLevel(2),
    m_zeroPadLevel(0),
    m_fftSize(1024),
    m_gain(1.0),
    m_initialGain(1.0),
    m_threshold(0.0),
    m_initialThreshold(0.0),
    m_colourRotation(0),
    m_initialRotation(0),
    m_minFrequency(10),
    m_maxFrequency(8000),
    m_initialMaxFrequency(8000),
    m_colourScale(dBColourScale),
    m_colourMap(0),
    m_frequencyScale(LinearFrequencyScale),
    m_binDisplay(AllBins),
    m_normalizeColumns(false),
    m_normalizeVisibleArea(false),
    m_lastEmittedZoomStep(-1),
    m_synchronous(false),
    m_lastPaintBlockWidth(0),
    m_updateTimer(0),
    m_candidateFillStartFrame(0),
    m_exiting(false),
    m_sliceableModel(0)
{
    if (config == FullRangeDb) {
        m_initialMaxFrequency = 0;
        setMaxFrequency(0);
    } else if (config == MelodicRange) {
	setWindowSize(8192);
	setWindowHopLevel(4);
        m_initialMaxFrequency = 1500;
	setMaxFrequency(1500);
        setMinFrequency(40);
	setColourScale(LinearColourScale);
        setColourMap(ColourMapper::Sunset);
        setFrequencyScale(LogFrequencyScale);
//        setGain(20);
    } else if (config == MelodicPeaks) {
	setWindowSize(4096);
	setWindowHopLevel(5);
        m_initialMaxFrequency = 2000;
	setMaxFrequency(2000);
	setMinFrequency(40);
	setFrequencyScale(LogFrequencyScale);
	setColourScale(LinearColourScale);
	setBinDisplay(PeakFrequencies);
	setNormalizeColumns(true);
    }

    Preferences *prefs = Preferences::getInstance();
    connect(prefs, SIGNAL(propertyChanged(PropertyContainer::PropertyName)),
            this, SLOT(preferenceChanged(PropertyContainer::PropertyName)));
    setWindowType(prefs->getWindowType());

    initialisePalette();
}

SpectrogramLayer::~SpectrogramLayer()
{
    delete m_updateTimer;
    m_updateTimer = 0;
    
    invalidateFFTModels();
}

void
SpectrogramLayer::setModel(const DenseTimeValueModel *model)
{
//    std::cerr << "SpectrogramLayer(" << this << "): setModel(" << model << ")" << std::endl;

    if (model == m_model) return;

    m_model = model;
    invalidateFFTModels();

    if (!m_model || !m_model->isOK()) return;

    connectSignals(m_model);

    connect(m_model, SIGNAL(modelChanged()), this, SLOT(cacheInvalid()));
    connect(m_model, SIGNAL(modelChanged(size_t, size_t)),
	    this, SLOT(cacheInvalid(size_t, size_t)));

    emit modelReplaced();
}

Layer::PropertyList
SpectrogramLayer::getProperties() const
{
    PropertyList list;
    list.push_back("Colour");
    list.push_back("Colour Scale");
    list.push_back("Window Size");
    list.push_back("Window Increment");
    list.push_back("Normalize Columns");
    list.push_back("Normalize Visible Area");
    list.push_back("Bin Display");
    list.push_back("Threshold");
    list.push_back("Gain");
    list.push_back("Colour Rotation");
//    list.push_back("Min Frequency");
//    list.push_back("Max Frequency");
    list.push_back("Frequency Scale");
////    list.push_back("Zero Padding");
    return list;
}

QString
SpectrogramLayer::getPropertyLabel(const PropertyName &name) const
{
    if (name == "Colour") return tr("Colour");
    if (name == "Colour Scale") return tr("Colour Scale");
    if (name == "Window Size") return tr("Window Size");
    if (name == "Window Increment") return tr("Window Overlap");
    if (name == "Normalize Columns") return tr("Normalize Columns");
    if (name == "Normalize Visible Area") return tr("Normalize Visible Area");
    if (name == "Bin Display") return tr("Bin Display");
    if (name == "Threshold") return tr("Threshold");
    if (name == "Gain") return tr("Gain");
    if (name == "Colour Rotation") return tr("Colour Rotation");
    if (name == "Min Frequency") return tr("Min Frequency");
    if (name == "Max Frequency") return tr("Max Frequency");
    if (name == "Frequency Scale") return tr("Frequency Scale");
    if (name == "Zero Padding") return tr("Smoothing");
    return "";
}

QString
SpectrogramLayer::getPropertyIconName(const PropertyName &name) const
{
    if (name == "Normalize Columns") return "normalise-columns";
    if (name == "Normalize Visible Area") return "normalise";
    return "";
}

Layer::PropertyType
SpectrogramLayer::getPropertyType(const PropertyName &name) const
{
    if (name == "Gain") return RangeProperty;
    if (name == "Colour Rotation") return RangeProperty;
    if (name == "Normalize Columns") return ToggleProperty;
    if (name == "Normalize Visible Area") return ToggleProperty;
    if (name == "Threshold") return RangeProperty;
    if (name == "Zero Padding") return ToggleProperty;
    return ValueProperty;
}

QString
SpectrogramLayer::getPropertyGroupName(const PropertyName &name) const
{
    if (name == "Bin Display" ||
        name == "Frequency Scale") return tr("Bins");
    if (name == "Window Size" ||
	name == "Window Increment" ||
        name == "Zero Padding") return tr("Window");
    if (name == "Colour" ||
	name == "Threshold" ||
	name == "Colour Rotation") return tr("Colour");
    if (name == "Normalize Columns" ||
        name == "Normalize Visible Area" ||
        name == "Gain" ||
	name == "Colour Scale") return tr("Scale");
    return QString();
}

int
SpectrogramLayer::getPropertyRangeAndValue(const PropertyName &name,
					   int *min, int *max, int *deflt) const
{
    int val = 0;

    int garbage0, garbage1, garbage2;
    if (!min) min = &garbage0;
    if (!max) max = &garbage1;
    if (!deflt) deflt = &garbage2;

    if (name == "Gain") {

	*min = -50;
	*max = 50;

        *deflt = lrintf(log10(m_initialGain) * 20.0);;
	if (*deflt < *min) *deflt = *min;
	if (*deflt > *max) *deflt = *max;

	val = lrintf(log10(m_gain) * 20.0);
	if (val < *min) val = *min;
	if (val > *max) val = *max;

    } else if (name == "Threshold") {

	*min = -50;
	*max = 0;

        *deflt = lrintf(AudioLevel::multiplier_to_dB(m_initialThreshold));
	if (*deflt < *min) *deflt = *min;
	if (*deflt > *max) *deflt = *max;

	val = lrintf(AudioLevel::multiplier_to_dB(m_threshold));
	if (val < *min) val = *min;
	if (val > *max) val = *max;

    } else if (name == "Colour Rotation") {

	*min = 0;
	*max = 256;
        *deflt = m_initialRotation;

	val = m_colourRotation;

    } else if (name == "Colour Scale") {

	*min = 0;
	*max = 4;
        *deflt = int(dBColourScale);

	val = (int)m_colourScale;

    } else if (name == "Colour") {

	*min = 0;
	*max = ColourMapper::getColourMapCount() - 1;
        *deflt = 0;

	val = m_colourMap;

    } else if (name == "Window Size") {

	*min = 0;
	*max = 10;
        *deflt = 5;
	
	val = 0;
	int ws = m_windowSize;
	while (ws > 32) { ws >>= 1; val ++; }

    } else if (name == "Window Increment") {
	
	*min = 0;
	*max = 5;
        *deflt = 2;

        val = m_windowHopLevel;
    
    } else if (name == "Zero Padding") {
	
	*min = 0;
	*max = 1;
        *deflt = 0;
	
        val = m_zeroPadLevel > 0 ? 1 : 0;
    
    } else if (name == "Min Frequency") {

	*min = 0;
	*max = 9;
        *deflt = 1;

	switch (m_minFrequency) {
	case 0: default: val = 0; break;
	case 10: val = 1; break;
	case 20: val = 2; break;
	case 40: val = 3; break;
	case 100: val = 4; break;
	case 250: val = 5; break;
	case 500: val = 6; break;
	case 1000: val = 7; break;
	case 4000: val = 8; break;
	case 10000: val = 9; break;
	}
    
    } else if (name == "Max Frequency") {

	*min = 0;
	*max = 9;
        *deflt = 6;

	switch (m_maxFrequency) {
	case 500: val = 0; break;
	case 1000: val = 1; break;
	case 1500: val = 2; break;
	case 2000: val = 3; break;
	case 4000: val = 4; break;
	case 6000: val = 5; break;
	case 8000: val = 6; break;
	case 12000: val = 7; break;
	case 16000: val = 8; break;
	default: val = 9; break;
	}

    } else if (name == "Frequency Scale") {

	*min = 0;
	*max = 1;
        *deflt = int(LinearFrequencyScale);
	val = (int)m_frequencyScale;

    } else if (name == "Bin Display") {

	*min = 0;
	*max = 2;
        *deflt = int(AllBins);
	val = (int)m_binDisplay;

    } else if (name == "Normalize Columns") {
	
        *deflt = 0;
	val = (m_normalizeColumns ? 1 : 0);

    } else if (name == "Normalize Visible Area") {
	
        *deflt = 0;
	val = (m_normalizeVisibleArea ? 1 : 0);

    } else {
	val = Layer::getPropertyRangeAndValue(name, min, max, deflt);
    }

    return val;
}

QString
SpectrogramLayer::getPropertyValueLabel(const PropertyName &name,
					int value) const
{
    if (name == "Colour") {
        return ColourMapper::getColourMapName(value);
    }
    if (name == "Colour Scale") {
	switch (value) {
	default:
	case 0: return tr("Linear");
	case 1: return tr("Meter");
	case 2: return tr("dBV^2");
	case 3: return tr("dBV");
	case 4: return tr("Phase");
	}
    }
    if (name == "Window Size") {
	return QString("%1").arg(32 << value);
    }
    if (name == "Window Increment") {
	switch (value) {
	default:
	case 0: return tr("None");
	case 1: return tr("25 %");
	case 2: return tr("50 %");
	case 3: return tr("75 %");
	case 4: return tr("87.5 %");
	case 5: return tr("93.75 %");
	}
    }
    if (name == "Zero Padding") {
        if (value == 0) return tr("None");
        return QString("%1x").arg(value + 1);
    }
    if (name == "Min Frequency") {
	switch (value) {
	default:
	case 0: return tr("No min");
	case 1: return tr("10 Hz");
	case 2: return tr("20 Hz");
	case 3: return tr("40 Hz");
	case 4: return tr("100 Hz");
	case 5: return tr("250 Hz");
	case 6: return tr("500 Hz");
	case 7: return tr("1 KHz");
	case 8: return tr("4 KHz");
	case 9: return tr("10 KHz");
	}
    }
    if (name == "Max Frequency") {
	switch (value) {
	default:
	case 0: return tr("500 Hz");
	case 1: return tr("1 KHz");
	case 2: return tr("1.5 KHz");
	case 3: return tr("2 KHz");
	case 4: return tr("4 KHz");
	case 5: return tr("6 KHz");
	case 6: return tr("8 KHz");
	case 7: return tr("12 KHz");
	case 8: return tr("16 KHz");
	case 9: return tr("No max");
	}
    }
    if (name == "Frequency Scale") {
	switch (value) {
	default:
	case 0: return tr("Linear");
	case 1: return tr("Log");
	}
    }
    if (name == "Bin Display") {
	switch (value) {
	default:
	case 0: return tr("All Bins");
	case 1: return tr("Peak Bins");
	case 2: return tr("Frequencies");
	}
    }
    return tr("<unknown>");
}

RangeMapper *
SpectrogramLayer::getNewPropertyRangeMapper(const PropertyName &name) const
{
    if (name == "Gain") {
        return new LinearRangeMapper(-50, 50, -25, 25, tr("dB"));
    }
    if (name == "Threshold") {
        return new LinearRangeMapper(-50, 0, -50, 0, tr("dB"));
    }
    return 0;
}

void
SpectrogramLayer::setProperty(const PropertyName &name, int value)
{
    if (name == "Gain") {
	setGain(pow(10, float(value)/20.0));
    } else if (name == "Threshold") {
	if (value == -50) setThreshold(0.0);
	else setThreshold(AudioLevel::dB_to_multiplier(value));
    } else if (name == "Colour Rotation") {
	setColourRotation(value);
    } else if (name == "Colour") {
        setColourMap(value);
    } else if (name == "Window Size") {
	setWindowSize(32 << value);
    } else if (name == "Window Increment") {
        setWindowHopLevel(value);
    } else if (name == "Zero Padding") {
        setZeroPadLevel(value > 0.1 ? 3 : 0);
    } else if (name == "Min Frequency") {
	switch (value) {
	default:
	case 0: setMinFrequency(0); break;
	case 1: setMinFrequency(10); break;
	case 2: setMinFrequency(20); break;
	case 3: setMinFrequency(40); break;
	case 4: setMinFrequency(100); break;
	case 5: setMinFrequency(250); break;
	case 6: setMinFrequency(500); break;
	case 7: setMinFrequency(1000); break;
	case 8: setMinFrequency(4000); break;
	case 9: setMinFrequency(10000); break;
	}
        int vs = getCurrentVerticalZoomStep();
        if (vs != m_lastEmittedZoomStep) {
            emit verticalZoomChanged();
            m_lastEmittedZoomStep = vs;
        }
    } else if (name == "Max Frequency") {
	switch (value) {
	case 0: setMaxFrequency(500); break;
	case 1: setMaxFrequency(1000); break;
	case 2: setMaxFrequency(1500); break;
	case 3: setMaxFrequency(2000); break;
	case 4: setMaxFrequency(4000); break;
	case 5: setMaxFrequency(6000); break;
	case 6: setMaxFrequency(8000); break;
	case 7: setMaxFrequency(12000); break;
	case 8: setMaxFrequency(16000); break;
	default:
	case 9: setMaxFrequency(0); break;
	}
        int vs = getCurrentVerticalZoomStep();
        if (vs != m_lastEmittedZoomStep) {
            emit verticalZoomChanged();
            m_lastEmittedZoomStep = vs;
        }
    } else if (name == "Colour Scale") {
	switch (value) {
	default:
	case 0: setColourScale(LinearColourScale); break;
	case 1: setColourScale(MeterColourScale); break;
	case 2: setColourScale(dBSquaredColourScale); break;
	case 3: setColourScale(dBColourScale); break;
	case 4: setColourScale(PhaseColourScale); break;
	}
    } else if (name == "Frequency Scale") {
	switch (value) {
	default:
	case 0: setFrequencyScale(LinearFrequencyScale); break;
	case 1: setFrequencyScale(LogFrequencyScale); break;
	}
    } else if (name == "Bin Display") {
	switch (value) {
	default:
	case 0: setBinDisplay(AllBins); break;
	case 1: setBinDisplay(PeakBins); break;
	case 2: setBinDisplay(PeakFrequencies); break;
	}
    } else if (name == "Normalize Columns") {
	setNormalizeColumns(value ? true : false);
    } else if (name == "Normalize Visible Area") {
	setNormalizeVisibleArea(value ? true : false);
    }
}

void
SpectrogramLayer::invalidateImageCaches()
{
    for (ViewImageCache::iterator i = m_imageCaches.begin();
         i != m_imageCaches.end(); ++i) {
        i->second.validArea = QRect();
    }
}

void
SpectrogramLayer::invalidateImageCaches(size_t startFrame, size_t endFrame)
{
    for (ViewImageCache::iterator i = m_imageCaches.begin();
         i != m_imageCaches.end(); ++i) {

        //!!! when are views removed from the map? on setLayerDormant?
        const View *v = i->first;

#ifdef DEBUG_SPECTROGRAM_REPAINT
        std::cerr << "SpectrogramLayer::invalidateImageCaches(" 
                  << startFrame << ", " << endFrame << "): view range is "
                  << v->getStartFrame() << ", " << v->getEndFrame()
                  << std::endl;

        std::cerr << "Valid area was: " << i->second.validArea.x() << ", "
                  << i->second.validArea.y() << " "
                  << i->second.validArea.width() << "x"
                  << i->second.validArea.height() << std::endl;
#endif

        if (long(startFrame) > v->getStartFrame()) {
            if (startFrame >= v->getEndFrame()) {
#ifdef DEBUG_SPECTROGRAM_REPAINT
                std::cerr << "Modified start frame is off right of view" << std::endl;
#endif
                return;
            }
            int x = v->getXForFrame(startFrame);
#ifdef DEBUG_SPECTROGRAM_REPAINT
            std::cerr << "clipping from 0 to " << x-1 << std::endl;
#endif
            if (x > 1) {
                i->second.validArea &=
                    QRect(0, 0, x-1, v->height());
            } else {
                i->second.validArea = QRect();
            }
        } else {
            if (long(endFrame) < v->getStartFrame()) {
#ifdef DEBUG_SPECTROGRAM_REPAINT
                std::cerr << "Modified end frame is off left of view" << std::endl;
#endif
                return;
            }
            int x = v->getXForFrame(endFrame);
#ifdef DEBUG_SPECTROGRAM_REPAINT
            std::cerr << "clipping from " << x+1 << " to " << v->width()
                      << std::endl;
#endif
            if (x < v->width()) {
                i->second.validArea &=
                    QRect(x+1, 0, v->width()-(x+1), v->height());
            } else {
                i->second.validArea = QRect();
            }
        }

#ifdef DEBUG_SPECTROGRAM_REPAINT
        std::cerr << "Valid area is now: " << i->second.validArea.x() << ", "
                  << i->second.validArea.y() << " "
                  << i->second.validArea.width() << "x"
                  << i->second.validArea.height() << std::endl;
#endif
    }
}

void
SpectrogramLayer::preferenceChanged(PropertyContainer::PropertyName name)
{
    std::cerr << "SpectrogramLayer::preferenceChanged(" << name.toStdString() << ")" << std::endl;

    if (name == "Window Type") {
        setWindowType(Preferences::getInstance()->getWindowType());
        return;
    }
    if (name == "Spectrogram Y Smoothing") {
        invalidateImageCaches();
        invalidateMagnitudes();
        emit layerParametersChanged();
    }
    if (name == "Spectrogram X Smoothing") {
        invalidateImageCaches();
        invalidateMagnitudes();
        emit layerParametersChanged();
    }
    if (name == "Tuning Frequency") {
        emit layerParametersChanged();
    }
}

void
SpectrogramLayer::setChannel(int ch)
{
    if (m_channel == ch) return;

    invalidateImageCaches();
    m_channel = ch;
    invalidateFFTModels();

    emit layerParametersChanged();
}

int
SpectrogramLayer::getChannel() const
{
    return m_channel;
}

void
SpectrogramLayer::setWindowSize(size_t ws)
{
    if (m_windowSize == ws) return;

    invalidateImageCaches();
    
    m_windowSize = ws;
    m_fftSize = ws * (m_zeroPadLevel + 1);
    
    invalidateFFTModels();

    emit layerParametersChanged();
}

size_t
SpectrogramLayer::getWindowSize() const
{
    return m_windowSize;
}

void
SpectrogramLayer::setWindowHopLevel(size_t v)
{
    if (m_windowHopLevel == v) return;

    invalidateImageCaches();
    
    m_windowHopLevel = v;
    
    invalidateFFTModels();

    emit layerParametersChanged();

//    fillCache();
}

size_t
SpectrogramLayer::getWindowHopLevel() const
{
    return m_windowHopLevel;
}

void
SpectrogramLayer::setZeroPadLevel(size_t v)
{
    if (m_zeroPadLevel == v) return;

    invalidateImageCaches();
    
    m_zeroPadLevel = v;
    m_fftSize = m_windowSize * (v + 1);

    invalidateFFTModels();

    emit layerParametersChanged();
}

size_t
SpectrogramLayer::getZeroPadLevel() const
{
    return m_zeroPadLevel;
}

void
SpectrogramLayer::setWindowType(WindowType w)
{
    if (m_windowType == w) return;

    invalidateImageCaches();
    
    m_windowType = w;

    invalidateFFTModels();

    emit layerParametersChanged();
}

WindowType
SpectrogramLayer::getWindowType() const
{
    return m_windowType;
}

void
SpectrogramLayer::setGain(float gain)
{
//    std::cerr << "SpectrogramLayer::setGain(" << gain << ") (my gain is now "
//	      << m_gain << ")" << std::endl;

    if (m_gain == gain) return;

    invalidateImageCaches();
    
    m_gain = gain;
    
    emit layerParametersChanged();
}

float
SpectrogramLayer::getGain() const
{
    return m_gain;
}

void
SpectrogramLayer::setThreshold(float threshold)
{
    if (m_threshold == threshold) return;

    invalidateImageCaches();
    
    m_threshold = threshold;

    emit layerParametersChanged();
}

float
SpectrogramLayer::getThreshold() const
{
    return m_threshold;
}

void
SpectrogramLayer::setMinFrequency(size_t mf)
{
    if (m_minFrequency == mf) return;

//    std::cerr << "SpectrogramLayer::setMinFrequency: " << mf << std::endl;

    invalidateImageCaches();
    invalidateMagnitudes();
    
    m_minFrequency = mf;

    emit layerParametersChanged();
}

size_t
SpectrogramLayer::getMinFrequency() const
{
    return m_minFrequency;
}

void
SpectrogramLayer::setMaxFrequency(size_t mf)
{
    if (m_maxFrequency == mf) return;

//    std::cerr << "SpectrogramLayer::setMaxFrequency: " << mf << std::endl;

    invalidateImageCaches();
    invalidateMagnitudes();
    
    m_maxFrequency = mf;
    
    emit layerParametersChanged();
}

size_t
SpectrogramLayer::getMaxFrequency() const
{
    return m_maxFrequency;
}

void
SpectrogramLayer::setColourRotation(int r)
{
    invalidateImageCaches();

    if (r < 0) r = 0;
    if (r > 256) r = 256;
    int distance = r - m_colourRotation;

    if (distance != 0) {
	rotatePalette(-distance);
	m_colourRotation = r;
    }
    
    emit layerParametersChanged();
}

void
SpectrogramLayer::setColourScale(ColourScale colourScale)
{
    if (m_colourScale == colourScale) return;

    invalidateImageCaches();
    
    m_colourScale = colourScale;
    
    emit layerParametersChanged();
}

SpectrogramLayer::ColourScale
SpectrogramLayer::getColourScale() const
{
    return m_colourScale;
}

void
SpectrogramLayer::setColourMap(int map)
{
    if (m_colourMap == map) return;

    invalidateImageCaches();
    
    m_colourMap = map;
    initialisePalette();

    emit layerParametersChanged();
}

int
SpectrogramLayer::getColourMap() const
{
    return m_colourMap;
}

void
SpectrogramLayer::setFrequencyScale(FrequencyScale frequencyScale)
{
    if (m_frequencyScale == frequencyScale) return;

    invalidateImageCaches();
    m_frequencyScale = frequencyScale;

    emit layerParametersChanged();
}

SpectrogramLayer::FrequencyScale
SpectrogramLayer::getFrequencyScale() const
{
    return m_frequencyScale;
}

void
SpectrogramLayer::setBinDisplay(BinDisplay binDisplay)
{
    if (m_binDisplay == binDisplay) return;

    invalidateImageCaches();
    m_binDisplay = binDisplay;

    emit layerParametersChanged();
}

SpectrogramLayer::BinDisplay
SpectrogramLayer::getBinDisplay() const
{
    return m_binDisplay;
}

void
SpectrogramLayer::setNormalizeColumns(bool n)
{
    if (m_normalizeColumns == n) return;

    invalidateImageCaches();
    invalidateMagnitudes();
    m_normalizeColumns = n;

    emit layerParametersChanged();
}

bool
SpectrogramLayer::getNormalizeColumns() const
{
    return m_normalizeColumns;
}

void
SpectrogramLayer::setNormalizeVisibleArea(bool n)
{
    std::cerr << "SpectrogramLayer::setNormalizeVisibleArea(" << n
              << ") (from " << m_normalizeVisibleArea << ")" << std::endl;

    if (m_normalizeVisibleArea == n) return;

    invalidateImageCaches();
    invalidateMagnitudes();
    m_normalizeVisibleArea = n;

    emit layerParametersChanged();
}

bool
SpectrogramLayer::getNormalizeVisibleArea() const
{
    return m_normalizeVisibleArea;
}

void
SpectrogramLayer::setLayerDormant(const View *v, bool dormant)
{
    if (dormant) {

#ifdef DEBUG_SPECTROGRAM_REPAINT
        std::cerr << "SpectrogramLayer::setLayerDormant(" << dormant << ")"
                  << std::endl;
#endif

        if (isLayerDormant(v)) {
            return;
        }

        Layer::setLayerDormant(v, true);

	invalidateImageCaches();
        m_imageCaches.erase(v);

        if (m_fftModels.find(v) != m_fftModels.end()) {

            if (m_sliceableModel == m_fftModels[v].first) {
                bool replaced = false;
                for (ViewFFTMap::iterator i = m_fftModels.begin();
                     i != m_fftModels.end(); ++i) {
                    if (i->second.first != m_sliceableModel) {
                        emit sliceableModelReplaced(m_sliceableModel, i->second.first);
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) emit sliceableModelReplaced(m_sliceableModel, 0);
            }

            delete m_fftModels[v].first;
            m_fftModels.erase(v);

            delete m_peakCaches[v];
            m_peakCaches.erase(v);
        }
	
    } else {

        Layer::setLayerDormant(v, false);
    }
}

void
SpectrogramLayer::cacheInvalid()
{
#ifdef DEBUG_SPECTROGRAM_REPAINT
    std::cerr << "SpectrogramLayer::cacheInvalid()" << std::endl;
#endif

    invalidateImageCaches();
    invalidateMagnitudes();
}

void
SpectrogramLayer::cacheInvalid(size_t from, size_t to)
{
#ifdef DEBUG_SPECTROGRAM_REPAINT
    std::cerr << "SpectrogramLayer::cacheInvalid(" << from << ", " << to << ")" << std::endl;
#endif

    invalidateImageCaches(from, to);
    invalidateMagnitudes();
}

void
SpectrogramLayer::fillTimerTimedOut()
{
    if (!m_model) return;

    bool allDone = true;

#ifdef DEBUG_SPECTROGRAM_REPAINT
    std::cerr << "SpectrogramLayer::fillTimerTimedOut: have " << m_fftModels.size() << " FFT models associated with views" << std::endl;
#endif

    for (ViewFFTMap::iterator i = m_fftModels.begin();
         i != m_fftModels.end(); ++i) {

        const FFTModel *model = i->second.first;
        size_t lastFill = i->second.second;

        if (model) {

            size_t fill = model->getFillExtent();

#ifdef DEBUG_SPECTROGRAM_REPAINT
            std::cerr << "SpectrogramLayer::fillTimerTimedOut: extent for " << model << ": " << fill << ", last " << lastFill << ", total " << m_model->getEndFrame() << std::endl;
#endif

            if (fill >= lastFill) {
                if (fill >= m_model->getEndFrame() && lastFill > 0) {
#ifdef DEBUG_SPECTROGRAM_REPAINT
                    std::cerr << "complete!" << std::endl;
#endif
                    invalidateImageCaches();
                    i->second.second = -1;
                    emit modelChanged();

                } else if (fill > lastFill) {
#ifdef DEBUG_SPECTROGRAM_REPAINT
                    std::cerr << "SpectrogramLayer: emitting modelChanged("
                              << lastFill << "," << fill << ")" << std::endl;
#endif
                    invalidateImageCaches(lastFill, fill);
                    i->second.second = fill;
                    emit modelChanged(lastFill, fill);
                }
            } else {
#ifdef DEBUG_SPECTROGRAM_REPAINT
                std::cerr << "SpectrogramLayer: going backwards, emitting modelChanged("
                          << m_model->getStartFrame() << "," << m_model->getEndFrame() << ")" << std::endl;
#endif
                invalidateImageCaches();
                i->second.second = fill;
                emit modelChanged(m_model->getStartFrame(), m_model->getEndFrame());
            }

            if (i->second.second >= 0) {
                allDone = false;
            }
        }
    }

    if (allDone) {
#ifdef DEBUG_SPECTROGRAM_REPAINT
        std::cerr << "SpectrogramLayer: all complete!" << std::endl;
#endif
        delete m_updateTimer;
        m_updateTimer = 0;
    }
}

bool
SpectrogramLayer::hasLightBackground() const 
{
    return ColourMapper(m_colourMap, 1.f, 255.f).hasLightBackground();
}

void
SpectrogramLayer::initialisePalette()
{
    int formerRotation = m_colourRotation;

    if (m_colourMap == (int)ColourMapper::BlackOnWhite) {
	m_palette.setColour(NO_VALUE, Qt::white);
    } else {
	m_palette.setColour(NO_VALUE, Qt::black);
    }

    ColourMapper mapper(m_colourMap, 1.f, 255.f);
    
    for (int pixel = 1; pixel < 256; ++pixel) {
        m_palette.setColour(pixel, mapper.map(pixel));
    }

    m_crosshairColour = mapper.getContrastingColour();

    m_colourRotation = 0;
    rotatePalette(m_colourRotation - formerRotation);
    m_colourRotation = formerRotation;

    m_drawBuffer = QImage();
}

void
SpectrogramLayer::rotatePalette(int distance)
{
    QColor newPixels[256];

    newPixels[NO_VALUE] = m_palette.getColour(NO_VALUE);

    for (int pixel = 1; pixel < 256; ++pixel) {
	int target = pixel + distance;
	while (target < 1) target += 255;
	while (target > 255) target -= 255;
	newPixels[target] = m_palette.getColour(pixel);
    }

    for (int pixel = 0; pixel < 256; ++pixel) {
	m_palette.setColour(pixel, newPixels[pixel]);
    }

    m_drawBuffer = QImage();
}

unsigned char
SpectrogramLayer::getDisplayValue(View *v, float input) const
{
    int value;

    float min = 0.f;
    float max = 1.f;

    if (m_normalizeVisibleArea) {
        min = m_viewMags[v].getMin();
        max = m_viewMags[v].getMax();
    } else if (!m_normalizeColumns) {
        if (m_colourScale == LinearColourScale //||
//            m_colourScale == MeterColourScale) {
            ) {
            max = 0.1f;
        }
    }

    float thresh = -80.f;

    if (max == 0.f) max = 1.f;
    if (max == min) min = max - 0.0001f;

    switch (m_colourScale) {
	
    default:
    case LinearColourScale:
        value = int(((input - min) / (max - min)) * 255.f) + 1;
	break;
	
    case MeterColourScale:
        value = AudioLevel::multiplier_to_preview
            ((input - min) / (max - min), 254) + 1;
	break;

    case dBSquaredColourScale:
        input = ((input - min) * (input - min)) / ((max - min) * (max - min));
        if (input > 0.f) {
            input = 10.f * log10f(input);
        } else {
            input = thresh;
        }
        if (min > 0.f) {
            thresh = 10.f * log10f(min * min);
            if (thresh < -80.f) thresh = -80.f;
        }
	input = (input - thresh) / (-thresh);
	if (input < 0.f) input = 0.f;
	if (input > 1.f) input = 1.f;
	value = int(input * 255.f) + 1;
	break;
	
    case dBColourScale:
        //!!! experiment with normalizing the visible area this way.
        //In any case, we need to have some indication of what the dB
        //scale is relative to.
        input = (input - min) / (max - min);
        if (input > 0.f) {
            input = 10.f * log10f(input);
        } else {
            input = thresh;
        }
        if (min > 0.f) {
            thresh = 10.f * log10f(min);
            if (thresh < -80.f) thresh = -80.f;
        }
	input = (input - thresh) / (-thresh);
	if (input < 0.f) input = 0.f;
	if (input > 1.f) input = 1.f;
	value = int(input * 255.f) + 1;
	break;
	
    case PhaseColourScale:
	value = int((input * 127.0 / M_PI) + 128);
	break;
    }

    if (value > UCHAR_MAX) value = UCHAR_MAX;
    if (value < 0) value = 0;
    return value;
}

float
SpectrogramLayer::getInputForDisplayValue(unsigned char uc) const
{
    //!!! unused

    int value = uc;
    float input;

    //!!! incorrect for normalizing visible area (and also out of date)
    
    switch (m_colourScale) {
	
    default:
    case LinearColourScale:
	input = float(value - 1) / 255.0 / (m_normalizeColumns ? 1 : 50);
	break;
    
    case MeterColourScale:
	input = AudioLevel::preview_to_multiplier(value - 1, 255)
	    / (m_normalizeColumns ? 1.0 : 50.0);
	break;

    case dBSquaredColourScale:
	input = float(value - 1) / 255.0;
	input = (input * 80.0) - 80.0;
	input = powf(10.0, input) / 20.0;
	value = int(input);
	break;

    case dBColourScale:
	input = float(value - 1) / 255.0;
	input = (input * 80.0) - 80.0;
	input = powf(10.0, input) / 20.0;
	value = int(input);
	break;

    case PhaseColourScale:
	input = float(value - 128) * M_PI / 127.0;
	break;
    }

    return input;
}

float
SpectrogramLayer::getEffectiveMinFrequency() const
{
    int sr = m_model->getSampleRate();
    float minf = float(sr) / m_fftSize;

    if (m_minFrequency > 0.0) {
	size_t minbin = size_t((double(m_minFrequency) * m_fftSize) / sr + 0.01);
	if (minbin < 1) minbin = 1;
	minf = minbin * sr / m_fftSize;
    }

    return minf;
}

float
SpectrogramLayer::getEffectiveMaxFrequency() const
{
    int sr = m_model->getSampleRate();
    float maxf = float(sr) / 2;

    if (m_maxFrequency > 0.0) {
	size_t maxbin = size_t((double(m_maxFrequency) * m_fftSize) / sr + 0.1);
	if (maxbin > m_fftSize / 2) maxbin = m_fftSize / 2;
	maxf = maxbin * sr / m_fftSize;
    }

    return maxf;
}

bool
SpectrogramLayer::getYBinRange(View *v, int y, float &q0, float &q1) const
{
    Profiler profiler("SpectrogramLayer::getYBinRange");
    
    int h = v->height();
    if (y < 0 || y >= h) return false;

    int sr = m_model->getSampleRate();
    float minf = getEffectiveMinFrequency();
    float maxf = getEffectiveMaxFrequency();

    bool logarithmic = (m_frequencyScale == LogFrequencyScale);

    q0 = v->getFrequencyForY(y, minf, maxf, logarithmic);
    q1 = v->getFrequencyForY(y - 1, minf, maxf, logarithmic);

    // Now map these on to ("proportions of") actual bins, using raw
    // FFT size (unsmoothed)

    q0 = (q0 * m_fftSize) / sr;
    q1 = (q1 * m_fftSize) / sr;

    return true;
}

bool
SpectrogramLayer::getSmoothedYBinRange(View *v, int y, float &q0, float &q1) const
{
    Profiler profiler("SpectrogramLayer::getSmoothedYBinRange");

    int h = v->height();
    if (y < 0 || y >= h) return false;

    int sr = m_model->getSampleRate();
    float minf = getEffectiveMinFrequency();
    float maxf = getEffectiveMaxFrequency();

    bool logarithmic = (m_frequencyScale == LogFrequencyScale);

    q0 = v->getFrequencyForY(y, minf, maxf, logarithmic);
    q1 = v->getFrequencyForY(y - 1, minf, maxf, logarithmic);

    // Now map these on to ("proportions of") actual bins, using raw
    // FFT size (unsmoothed)

    q0 = (q0 * getFFTSize(v)) / sr;
    q1 = (q1 * getFFTSize(v)) / sr;

    return true;
}
    
bool
SpectrogramLayer::getXBinRange(View *v, int x, float &s0, float &s1) const
{
    size_t modelStart = m_model->getStartFrame();
    size_t modelEnd = m_model->getEndFrame();

    // Each pixel column covers an exact range of sample frames:
    int f0 = v->getFrameForX(x) - modelStart;
    int f1 = v->getFrameForX(x + 1) - modelStart - 1;

    if (f1 < int(modelStart) || f0 > int(modelEnd)) {
	return false;
    }
      
    // And that range may be drawn from a possibly non-integral
    // range of spectrogram windows:

    size_t windowIncrement = getWindowIncrement();
    s0 = float(f0) / windowIncrement;
    s1 = float(f1) / windowIncrement;

    return true;
}
 
bool
SpectrogramLayer::getXBinSourceRange(View *v, int x, RealTime &min, RealTime &max) const
{
    float s0 = 0, s1 = 0;
    if (!getXBinRange(v, x, s0, s1)) return false;
    
    int s0i = int(s0 + 0.001);
    int s1i = int(s1);

    int windowIncrement = getWindowIncrement();
    int w0 = s0i * windowIncrement - (m_windowSize - windowIncrement)/2;
    int w1 = s1i * windowIncrement + windowIncrement +
	(m_windowSize - windowIncrement)/2 - 1;
    
    min = RealTime::frame2RealTime(w0, m_model->getSampleRate());
    max = RealTime::frame2RealTime(w1, m_model->getSampleRate());
    return true;
}

bool
SpectrogramLayer::getYBinSourceRange(View *v, int y, float &freqMin, float &freqMax)
const
{
    float q0 = 0, q1 = 0;
    if (!getYBinRange(v, y, q0, q1)) return false;

    int q0i = int(q0 + 0.001);
    int q1i = int(q1);

    int sr = m_model->getSampleRate();

    for (int q = q0i; q <= q1i; ++q) {
	if (q == q0i) freqMin = (sr * q) / m_fftSize;
	if (q == q1i) freqMax = (sr * (q+1)) / m_fftSize;
    }
    return true;
}

bool
SpectrogramLayer::getAdjustedYBinSourceRange(View *v, int x, int y,
					     float &freqMin, float &freqMax,
					     float &adjFreqMin, float &adjFreqMax)
const
{
    if (!m_model || !m_model->isOK() || !m_model->isReady()) {
	return false;
    }

    FFTModel *fft = getFFTModel(v);
    if (!fft) return false;

    float s0 = 0, s1 = 0;
    if (!getXBinRange(v, x, s0, s1)) return false;

    float q0 = 0, q1 = 0;
    if (!getYBinRange(v, y, q0, q1)) return false;

    int s0i = int(s0 + 0.001);
    int s1i = int(s1);

    int q0i = int(q0 + 0.001);
    int q1i = int(q1);

    int sr = m_model->getSampleRate();

    size_t windowSize = m_windowSize;
    size_t windowIncrement = getWindowIncrement();

    bool haveAdj = false;

    bool peaksOnly = (m_binDisplay == PeakBins ||
		      m_binDisplay == PeakFrequencies);

    for (int q = q0i; q <= q1i; ++q) {

	for (int s = s0i; s <= s1i; ++s) {

            if (!fft->isColumnAvailable(s)) continue;

	    float binfreq = (sr * q) / m_windowSize;
	    if (q == q0i) freqMin = binfreq;
	    if (q == q1i) freqMax = binfreq;

	    if (peaksOnly && !fft->isLocalPeak(s, q)) continue;

	    if (!fft->isOverThreshold(s, q, m_threshold * (m_fftSize/2))) continue;

	    float freq = binfreq;
	    bool steady = false;
	    
	    if (s < int(fft->getWidth()) - 1) {

                fft->estimateStableFrequency(s, q, freq);
	    
		if (!haveAdj || freq < adjFreqMin) adjFreqMin = freq;
		if (!haveAdj || freq > adjFreqMax) adjFreqMax = freq;

		haveAdj = true;
	    }
	}
    }

    if (!haveAdj) {
	adjFreqMin = adjFreqMax = 0.0;
    }

    return haveAdj;
}
    
bool
SpectrogramLayer::getXYBinSourceRange(View *v, int x, int y,
				      float &min, float &max,
				      float &phaseMin, float &phaseMax) const
{
    if (!m_model || !m_model->isOK() || !m_model->isReady()) {
	return false;
    }

    float q0 = 0, q1 = 0;
    if (!getYBinRange(v, y, q0, q1)) return false;

    float s0 = 0, s1 = 0;
    if (!getXBinRange(v, x, s0, s1)) return false;
    
    int q0i = int(q0 + 0.001);
    int q1i = int(q1);

    int s0i = int(s0 + 0.001);
    int s1i = int(s1);

    bool rv = false;

    size_t zp = getZeroPadLevel(v);
    q0i *= zp + 1;
    q1i *= zp + 1;

    FFTModel *fft = getFFTModel(v);

    if (fft) {

        int cw = fft->getWidth();
        int ch = fft->getHeight();

        min = 0.0;
        max = 0.0;
        phaseMin = 0.0;
        phaseMax = 0.0;
        bool have = false;

        for (int q = q0i; q <= q1i; ++q) {
            for (int s = s0i; s <= s1i; ++s) {
                if (s >= 0 && q >= 0 && s < cw && q < ch) {

                    if (!fft->isColumnAvailable(s)) continue;
                    
                    float value;

                    value = fft->getPhaseAt(s, q);
                    if (!have || value < phaseMin) { phaseMin = value; }
                    if (!have || value > phaseMax) { phaseMax = value; }

                    value = fft->getMagnitudeAt(s, q) / (m_fftSize/2);
                    if (!have || value < min) { min = value; }
                    if (!have || value > max) { max = value; }
                    
                    have = true;
                }	
            }
        }
        
        if (have) {
            rv = true;
        }
    }

    return rv;
}
   
size_t
SpectrogramLayer::getZeroPadLevel(const View *v) const
{
    //!!! tidy all this stuff

    if (m_binDisplay != AllBins) return 0;

    Preferences::SpectrogramSmoothing smoothing = 
        Preferences::getInstance()->getSpectrogramSmoothing();
    
    if (smoothing == Preferences::NoSpectrogramSmoothing ||
        smoothing == Preferences::SpectrogramInterpolated) return 0;

    if (m_frequencyScale == LogFrequencyScale) return 3;

    int sr = m_model->getSampleRate();
    
    size_t maxbin = m_fftSize / 2;
    if (m_maxFrequency > 0) {
	maxbin = int((double(m_maxFrequency) * m_fftSize) / sr + 0.1);
	if (maxbin > m_fftSize / 2) maxbin = m_fftSize / 2;
    }

    size_t minbin = 1;
    if (m_minFrequency > 0) {
	minbin = int((double(m_minFrequency) * m_fftSize) / sr + 0.1);
	if (minbin < 1) minbin = 1;
	if (minbin >= maxbin) minbin = maxbin - 1;
    }

    float perPixel =
        float(v->height()) /
        float((maxbin - minbin) / (m_zeroPadLevel + 1));

    if (perPixel > 2.8) {
        return 3; // 4x oversampling
    } else if (perPixel > 1.5) {
        return 1; // 2x
    } else {
        return 0; // 1x
    }
}

size_t
SpectrogramLayer::getFFTSize(const View *v) const
{
    return m_fftSize * (getZeroPadLevel(v) + 1);
}
	
FFTModel *
SpectrogramLayer::getFFTModel(const View *v) const
{
    if (!m_model) return 0;

    size_t fftSize = getFFTSize(v);

    if (m_fftModels.find(v) != m_fftModels.end()) {
        if (m_fftModels[v].first == 0) {
#ifdef DEBUG_SPECTROGRAM_REPAINT
            std::cerr << "SpectrogramLayer::getFFTModel(" << v << "): Found null model" << std::endl;
#endif
            return 0;
        }
        if (m_fftModels[v].first->getHeight() != fftSize / 2 + 1) {
#ifdef DEBUG_SPECTROGRAM_REPAINT
            std::cerr << "SpectrogramLayer::getFFTModel(" << v << "): Found a model with the wrong height (" << m_fftModels[v].first->getHeight() << ", wanted " << (fftSize / 2 + 1) << ")" << std::endl;
#endif
            delete m_fftModels[v].first;
            m_fftModels.erase(v);
            delete m_peakCaches[v];
            m_peakCaches.erase(v);
        } else {
#ifdef DEBUG_SPECTROGRAM_REPAINT
            std::cerr << "SpectrogramLayer::getFFTModel(" << v << "): Found a good model of height " << m_fftModels[v].first->getHeight() << std::endl;
#endif
            return m_fftModels[v].first;
        }
    }

    if (m_fftModels.find(v) == m_fftModels.end()) {

        FFTModel *model = new FFTModel(m_model,
                                       m_channel,
                                       m_windowType,
                                       m_windowSize,
                                       getWindowIncrement(),
                                       fftSize,
                                       true, // polar
                                       StorageAdviser::SpeedCritical,
                                       m_candidateFillStartFrame);

        if (!model->isOK()) {
            QMessageBox::critical
                (0, tr("FFT cache failed"),
                 tr("Failed to create the FFT model for this spectrogram.\n"
                    "There may be insufficient memory or disc space to continue."));
            delete model;
            m_fftModels[v] = FFTFillPair(0, 0);
            return 0;
        }

        if (!m_sliceableModel) {
#ifdef DEBUG_SPECTROGRAM
            std::cerr << "SpectrogramLayer: emitting sliceableModelReplaced(0, " << model << ")" << std::endl;
#endif
            ((SpectrogramLayer *)this)->sliceableModelReplaced(0, model);
            m_sliceableModel = model;
        }

        m_fftModels[v] = FFTFillPair(model, 0);

        model->resume();
        
        delete m_updateTimer;
        m_updateTimer = new QTimer((SpectrogramLayer *)this);
        connect(m_updateTimer, SIGNAL(timeout()),
                this, SLOT(fillTimerTimedOut()));
        m_updateTimer->start(200);
    }

    return m_fftModels[v].first;
}

Dense3DModelPeakCache *
SpectrogramLayer::getPeakCache(const View *v) const
{
    if (!m_peakCaches[v]) {
        FFTModel *f = getFFTModel(v);
        if (!f) return 0;
        m_peakCaches[v] = new Dense3DModelPeakCache(f, 8);
    }
    return m_peakCaches[v];
}

const Model *
SpectrogramLayer::getSliceableModel() const
{
    if (m_sliceableModel) return m_sliceableModel;
    if (m_fftModels.empty()) return 0;
    m_sliceableModel = m_fftModels.begin()->second.first;
    return m_sliceableModel;
}

void
SpectrogramLayer::invalidateFFTModels()
{
    for (ViewFFTMap::iterator i = m_fftModels.begin();
         i != m_fftModels.end(); ++i) {
        delete i->second.first;
    }
    for (PeakCacheMap::iterator i = m_peakCaches.begin();
         i != m_peakCaches.end(); ++i) {
        delete i->second;
    }
    
    m_fftModels.clear();
    m_peakCaches.clear();

    if (m_sliceableModel) {
        std::cerr << "SpectrogramLayer: emitting sliceableModelReplaced(" << m_sliceableModel << ", 0)" << std::endl;
        emit sliceableModelReplaced(m_sliceableModel, 0);
        m_sliceableModel = 0;
    }
}

void
SpectrogramLayer::invalidateMagnitudes()
{
    m_viewMags.clear();
    for (std::vector<MagnitudeRange>::iterator i = m_columnMags.begin();
         i != m_columnMags.end(); ++i) {
        *i = MagnitudeRange();
    }
}

bool
SpectrogramLayer::updateViewMagnitudes(View *v) const
{
    MagnitudeRange mag;

    int x0 = 0, x1 = v->width();
    float s00 = 0, s01 = 0, s10 = 0, s11 = 0;
    
    if (!getXBinRange(v, x0, s00, s01)) {
        s00 = s01 = m_model->getStartFrame() / getWindowIncrement();
    }

    if (!getXBinRange(v, x1, s10, s11)) {
        s10 = s11 = m_model->getEndFrame() / getWindowIncrement();
    }

    int s0 = int(std::min(s00, s10) + 0.0001);
    int s1 = int(std::max(s01, s11) + 0.0001);

//    std::cerr << "SpectrogramLayer::updateViewMagnitudes: x0 = " << x0 << ", x1 = " << x1 << ", s00 = " << s00 << ", s11 = " << s11 << " s0 = " << s0 << ", s1 = " << s1 << std::endl;

    if (int(m_columnMags.size()) <= s1) {
        m_columnMags.resize(s1 + 1);
    }

    for (int s = s0; s <= s1; ++s) {
        if (m_columnMags[s].isSet()) {
            mag.sample(m_columnMags[s]);
        }
    }

#ifdef DEBUG_SPECTROGRAM_REPAINT
    std::cerr << "SpectrogramLayer::updateViewMagnitudes returning from cols "
              << s0 << " -> " << s1 << " inclusive" << std::endl;
#endif

    if (!mag.isSet()) return false;
    if (mag == m_viewMags[v]) return false;
    m_viewMags[v] = mag;
    return true;
}

void
SpectrogramLayer::setSynchronousPainting(bool synchronous)
{
    m_synchronous = synchronous;
}

void
SpectrogramLayer::paint(View *v, QPainter &paint, QRect rect) const
{
    // What a lovely, old-fashioned function this is.
    // It's practically FORTRAN 77 in its clarity and linearity.

    Profiler profiler("SpectrogramLayer::paint", false);

#ifdef DEBUG_SPECTROGRAM_REPAINT
    std::cerr << "SpectrogramLayer::paint(): m_model is " << m_model << ", zoom level is " << v->getZoomLevel() << ", m_updateTimer " << m_updateTimer << std::endl;
    
    std::cerr << "rect is " << rect.x() << "," << rect.y() << " " << rect.width() << "x" << rect.height() << std::endl;
#endif

    long startFrame = v->getStartFrame();
    if (startFrame < 0) m_candidateFillStartFrame = 0;
    else m_candidateFillStartFrame = startFrame;

    if (!m_model || !m_model->isOK() || !m_model->isReady()) {
	return;
    }

    if (isLayerDormant(v)) {
	std::cerr << "SpectrogramLayer::paint(): Layer is dormant, making it undormant again" << std::endl;
    }

    // Need to do this even if !isLayerDormant, as that could mean v
    // is not in the dormancy map at all -- we need it to be present
    // and accountable for when determining whether we need the cache
    // in the cache-fill thread above.
    //!!! no longer use cache-fill thread
    const_cast<SpectrogramLayer *>(this)->Layer::setLayerDormant(v, false);

    size_t fftSize = getFFTSize(v);
/*
    FFTModel *fft = getFFTModel(v);
    if (!fft) {
	std::cerr << "ERROR: SpectrogramLayer::paint(): No FFT model, returning" << std::endl;
	return;
    }
*/
    ImageCache &cache = m_imageCaches[v];

#ifdef DEBUG_SPECTROGRAM_REPAINT
    std::cerr << "SpectrogramLayer::paint(): image cache valid area " << cache.

validArea.x() << ", " << cache.validArea.y() << ", " << cache.validArea.width() << "x" << cache.validArea.height() << std::endl;
#endif

#ifdef DEBUG_SPECTROGRAM_REPAINT
    bool stillCacheing = (m_updateTimer != 0);
    std::cerr << "SpectrogramLayer::paint(): Still cacheing = " << stillCacheing << std::endl;
#endif

    int zoomLevel = v->getZoomLevel();

    int x0 = 0;
    int x1 = v->width();

    bool recreateWholeImageCache = true;

    x0 = rect.left();
    x1 = rect.right() + 1;
/*
    float xPixelRatio = float(fft->getResolution()) / float(zoomLevel);
    std::cerr << "xPixelRatio = " << xPixelRatio << std::endl;
    if (xPixelRatio < 1.f) xPixelRatio = 1.f;
*/
    if (cache.validArea.width() > 0) {

        int cw = cache.image.width();
        int ch = cache.image.height();
        
	if (int(cache.zoomLevel) == zoomLevel &&
	    cw == v->width() &&
	    ch == v->height()) {

	    if (v->getXForFrame(cache.startFrame) ==
		v->getXForFrame(startFrame) &&
                cache.validArea.x() <= x0 &&
                cache.validArea.x() + cache.validArea.width() >= x1) {
	    
#ifdef DEBUG_SPECTROGRAM_REPAINT
		std::cerr << "SpectrogramLayer: image cache good" << std::endl;
#endif

		paint.drawImage(rect, cache.image, rect);
                //!!!
//                paint.drawImage(v->rect(), cache.image,
//                                QRect(QPoint(0, 0), cache.image.size()));

                illuminateLocalFeatures(v, paint);
		return;

	    } else {

#ifdef DEBUG_SPECTROGRAM_REPAINT
		std::cerr << "SpectrogramLayer: image cache partially OK" << std::endl;
#endif

		recreateWholeImageCache = false;

		int dx = v->getXForFrame(cache.startFrame) -
		         v->getXForFrame(startFrame);

#ifdef DEBUG_SPECTROGRAM_REPAINT
		std::cerr << "SpectrogramLayer: dx = " << dx << " (image cache " << cw << "x" << ch << ")" << std::endl;
#endif

		if (dx != 0 &&
                    dx > -cw &&
                    dx <  cw) {
                    
                    int dxp = dx;
                    if (dxp < 0) dxp = -dxp;
                    int copy = (cw - dxp) * sizeof(QRgb);
                    for (int y = 0; y < ch; ++y) {
                        QRgb *line = (QRgb *)cache.image.scanLine(y);
                        if (dx < 0) {
                            memmove(line, line + dxp, copy);
                        } else {
                            memmove(line + dxp, line, copy);
                        }
                    }

                    int px = cache.validArea.x();
                    int pw = cache.validArea.width();

		    if (dx < 0) {
			x0 = cw + dx;
			x1 = cw;
                        px += dx;
                        if (px < 0) {
                            pw += px;
                            px = 0;
                            if (pw < 0) pw = 0;
                        }
		    } else {
			x0 = 0;
			x1 = dx;
                        px += dx;
                        if (px + pw > cw) {
                            pw = int(cw) - px;
                            if (pw < 0) pw = 0;
                        }
		    }
                    
                    cache.validArea =
                        QRect(px, cache.validArea.y(),
                              pw, cache.validArea.height());

#ifdef DEBUG_SPECTROGRAM_REPAINT
                    std::cerr << "valid area now "
                              << px << "," << cache.validArea.y()
                              << " " << pw << "x" << cache.validArea.height()
                              << std::endl;
#endif
/*
		    paint.drawImage(rect & cache.validArea,
                                     cache.image,
                                     rect & cache.validArea);
*/
                } else if (dx != 0) {

                    // we scrolled too far to be of use

#ifdef DEBUG_SPECTROGRAM_REPAINT
                    std::cerr << "dx == " << dx << ": scrolled too far for cache to be useful" << std::endl;
#endif

                    cache.validArea = QRect();
                    recreateWholeImageCache = true;
                }
	    }
	} else {
#ifdef DEBUG_SPECTROGRAM_REPAINT
	    std::cerr << "SpectrogramLayer: image cache useless" << std::endl;
            if (int(cache.zoomLevel) != zoomLevel) {
                std::cerr << "(cache zoomLevel " << cache.zoomLevel
                          << " != " << zoomLevel << ")" << std::endl;
            }
            if (cw != v->width()) {
                std::cerr << "(cache width " << cw
                          << " != " << v->width();
            }
            if (ch != v->height()) {
                std::cerr << "(cache height " << ch
                          << " != " << v->height();
            }
#endif
            cache.validArea = QRect();
//            recreateWholeImageCache = true;
	}
    }

    if (updateViewMagnitudes(v)) {
#ifdef DEBUG_SPECTROGRAM_REPAINT
        std::cerr << "SpectrogramLayer: magnitude range changed to [" << m_viewMags[v].getMin() << "->" << m_viewMags[v].getMax() << "]" << std::endl;
#endif
        if (m_normalizeVisibleArea) {
            cache.validArea = QRect();
            recreateWholeImageCache = true;
        }
    } else {
#ifdef DEBUG_SPECTROGRAM_REPAINT
        std::cerr << "No change in magnitude range [" << m_viewMags[v].getMin() << "->" << m_viewMags[v].getMax() << "]" << std::endl;
#endif
    }

    if (recreateWholeImageCache) {
        x0 = 0;
        x1 = v->width();
    }

    struct timeval tv;
    (void)gettimeofday(&tv, 0);
    RealTime mainPaintStart = RealTime::fromTimeval(tv);

    int paintBlockWidth = m_lastPaintBlockWidth;

    if (m_synchronous) {
        if (paintBlockWidth < x1 - x0) {
            // always paint full width
            paintBlockWidth = x1 - x0;
        }
    } else {
        if (paintBlockWidth == 0) {
            paintBlockWidth = (300000 / zoomLevel);
        } else {
            RealTime lastTime = m_lastPaintTime;
            while (lastTime > RealTime::fromMilliseconds(200) &&
                   paintBlockWidth > 50) {
                paintBlockWidth /= 2;
                lastTime = lastTime / 2;
            }
            while (lastTime < RealTime::fromMilliseconds(90) &&
                   paintBlockWidth < 1500) {
                paintBlockWidth *= 2;
                lastTime = lastTime * 2;
            }
        }
        
        if (paintBlockWidth < 20) paintBlockWidth = 20;
    }

#ifdef DEBUG_SPECTROGRAM_REPAINT
    std::cerr << "[" << this << "]: last paint width: " << m_lastPaintBlockWidth << ", last paint time: " << m_lastPaintTime << ", new paint width: " << paintBlockWidth << std::endl;
#endif

    // We always paint the full height when refreshing the cache.
    // Smaller heights can be used when painting direct from cache
    // (further up in this function), but we want to ensure the cache
    // is coherent without having to worry about vertical matching of
    // required and valid areas as well as horizontal.

    int h = v->height();

    if (cache.validArea.width() > 0) {

        // If part of the cache is known to be valid, select a strip
        // immediately to left or right of the valid part

        //!!! this really needs to be coordinated with the selection
        //!!! of m_drawBuffer boundaries in the bufferBinResolution
        //!!! case below

        int vx0 = 0, vx1 = 0;
        vx0 = cache.validArea.x();
        vx1 = cache.validArea.x() + cache.validArea.width();
        
#ifdef DEBUG_SPECTROGRAM_REPAINT
        std::cerr << "x0 " << x0 << ", x1 " << x1 << ", vx0 " << vx0 << ", vx1 " << vx1 << ", paintBlockWidth " << paintBlockWidth << std::endl;
#endif         
        if (x0 < vx0) {
            if (x0 + paintBlockWidth < vx0) {
                x0 = vx0 - paintBlockWidth;
            }
            x1 = vx0;
        } else if (x0 >= vx1) {
            x0 = vx1;
            if (x1 > x0 + paintBlockWidth) {
                x1 = x0 + paintBlockWidth;
            }
        } else {
            // x0 is within the valid area
            if (x1 > vx1) {
                x0 = vx1;
                if (x0 + paintBlockWidth < x1) {
                    x1 = x0 + paintBlockWidth;
                }
            } else {
                x1 = x0; // it's all valid, paint nothing
            }
        }
         
        cache.validArea = QRect
            (std::min(vx0, x0), cache.validArea.y(),
             std::max(vx1 - std::min(vx0, x0),
                       x1 - std::min(vx0, x0)),
             cache.validArea.height());

#ifdef DEBUG_SPECTROGRAM_REPAINT
        std::cerr << "Valid area becomes " << cache.validArea.x()
                  << ", " << cache.validArea.y() << ", "
                  << cache.validArea.width() << "x"
                  << cache.validArea.height() << std::endl;
#endif
            
    } else {
        if (x1 > x0 + paintBlockWidth) {
            int sfx = x1;
            if (startFrame < 0) sfx = v->getXForFrame(0);
            if (sfx >= x0 && sfx + paintBlockWidth <= x1) {
                x0 = sfx;
                x1 = x0 + paintBlockWidth;
            } else {
                int mid = (x1 + x0) / 2;
                x0 = mid - paintBlockWidth/2;
                x1 = x0 + paintBlockWidth;
            }
        }
#ifdef DEBUG_SPECTROGRAM_REPAINT
        std::cerr << "Valid area becomes " << x0 << ", 0, " << (x1-x0)
                  << "x" << h << std::endl;
#endif
        cache.validArea = QRect(x0, 0, x1 - x0, h);
    }

/*
    if (xPixelRatio != 1.f) {
        x0 = int((int(x0 / xPixelRatio) - 4) * xPixelRatio + 0.0001);
        x1 = int((int(x1 / xPixelRatio) + 4) * xPixelRatio + 0.0001);
    }
*/
    int w = x1 - x0;

#ifdef DEBUG_SPECTROGRAM_REPAINT
    std::cerr << "x0 " << x0 << ", x1 " << x1 << ", w " << w << ", h " << h << std::endl;
#endif

    int sr = m_model->getSampleRate();

    // Set minFreq and maxFreq to the frequency extents of the possibly
    // zero-padded visible bin range, and displayMinFreq and displayMaxFreq
    // to the actual scale frequency extents (presumably not zero padded).

    // If we are zero padding, we want to use the zero-padded
    // equivalents of the bins that we would be using if not zero
    // padded, to avoid spaces at the top and bottom of the display.

    // Note fftSize is the actual zero-padded fft size, m_fftSize the
    // nominal fft size.
    
    size_t maxbin = m_fftSize / 2;
    if (m_maxFrequency > 0) {
	maxbin = int((double(m_maxFrequency) * m_fftSize) / sr + 0.001);
	if (maxbin > m_fftSize / 2) maxbin = m_fftSize / 2;
    }

    size_t minbin = 1;
    if (m_minFrequency > 0) {
	minbin = int((double(m_minFrequency) * m_fftSize) / sr + 0.001);
//        std::cerr << "m_minFrequency = " << m_minFrequency << " -> minbin = " << minbin << std::endl;
	if (minbin < 1) minbin = 1;
	if (minbin >= maxbin) minbin = maxbin - 1;
    }

    int zpl = getZeroPadLevel(v) + 1;
    minbin = minbin * zpl;
    maxbin = (maxbin + 1) * zpl - 1;

    float minFreq = (float(minbin) * sr) / fftSize;
    float maxFreq = (float(maxbin) * sr) / fftSize;

    float displayMinFreq = minFreq;
    float displayMaxFreq = maxFreq;

    if (fftSize != m_fftSize) {
        displayMinFreq = getEffectiveMinFrequency();
        displayMaxFreq = getEffectiveMaxFrequency();
    }

//    std::cerr << "(giving actual minFreq " << minFreq << " and display minFreq " << displayMinFreq << ")" << std::endl;

    int increment = getWindowIncrement();
    
    bool logarithmic = (m_frequencyScale == LogFrequencyScale);
/*
    float yforbin[maxbin - minbin + 1];

    for (size_t q = minbin; q <= maxbin; ++q) {
        float f0 = (float(q) * sr) / fftSize;
        yforbin[q - minbin] =
            v->getYForFrequency(f0, displayMinFreq, displayMaxFreq,
                                logarithmic);
    }
*/
    MagnitudeRange overallMag = m_viewMags[v];
    bool overallMagChanged = false;

    bool fftSuspended = false;

#ifdef DEBUG_SPECTROGRAM_REPAINT
    std::cerr << ((float(v->getFrameForX(1) - v->getFrameForX(0))) / increment) << " bin(s) per pixel" << std::endl;
#endif

    bool runOutOfData = false;

    if (w == 0) {
        std::cerr << "*** NOTE: w == 0" << std::endl;
    }

#ifdef DEBUG_SPECTROGRAM_REPAINT
    size_t pixels = 0;
#endif

    Profiler outerprof("SpectrogramLayer::paint: all cols");

    // The draw buffer contains a fragment at either our pixel
    // resolution (if there is more than one time-bin per pixel) or
    // time-bin resolution (if a time-bin spans more than one pixel).
    // We need to ensure that it starts and ends at points where a
    // time-bin boundary occurs at an exact pixel boundary, and with a
    // certain amount of overlap across existing pixels so that we can
    // scale and draw from it without smoothing errors at the edges.

    // If (getFrameForX(x) / increment) * increment ==
    // getFrameForX(x), then x is a time-bin boundary.  We want two
    // such boundaries at either side of the draw buffer -- one which
    // we draw up to, and one which we subsequently crop at.

    bool bufferBinResolution = false;
    if (increment > zoomLevel) bufferBinResolution = true;

    long leftBoundaryFrame = -1, leftCropFrame = -1;
    long rightBoundaryFrame = -1, rightCropFrame = -1;

    int bufwid;

    if (bufferBinResolution) {

        for (int x = x0; ; --x) {
            long f = v->getFrameForX(x);
            if ((f / increment) * increment == f) {
                if (leftCropFrame == -1) leftCropFrame = f;
                else if (x < x0 - 2) { leftBoundaryFrame = f; break; }
            }
        }
        for (int x = x0 + w; ; ++x) {
            long f = v->getFrameForX(x);
            if ((f / increment) * increment == f) {
                if (rightCropFrame == -1) rightCropFrame = f;
                else if (x > x0 + w + 2) { rightBoundaryFrame = f; break; }
            }
        }
#ifdef DEBUG_SPECTROGRAM_REPAINT
        cerr << "Left: crop: " << leftCropFrame << " (bin " << leftCropFrame/increment << "); boundary: " << leftBoundaryFrame << " (bin " << leftBoundaryFrame/increment << ")" << endl;
        cerr << "Right: crop: " << rightCropFrame << " (bin " << rightCropFrame/increment << "); boundary: " << rightBoundaryFrame << " (bin " << rightBoundaryFrame/increment << ")" << endl;
#endif

        bufwid = (rightBoundaryFrame - leftBoundaryFrame) / increment;

    } else {
        
        bufwid = w;
    }

#ifdef __GNUC__
    int binforx[bufwid];
    float binfory[h];
#else
    int *binforx = (int *)alloca(bufwid * sizeof(int));
    float *binfory = (float *)alloca(h * sizeof(float));
#endif

    bool usePeaksCache = false;

    if (bufferBinResolution) {
        for (int x = 0; x < bufwid; ++x) {
            binforx[x] = (leftBoundaryFrame / increment) + x;
//            cerr << "binforx[" << x << "] = " << binforx[x] << endl;
        }
        m_drawBuffer = QImage(bufwid, h, QImage::Format_Indexed8);
    } else {
        for (int x = 0; x < bufwid; ++x) {
            float s0 = 0, s1 = 0;
            if (getXBinRange(v, x + x0, s0, s1)) {
                binforx[x] = int(s0 + 0.0001);
            } else {
                binforx[x] = -1; //???
            }
        }
        if (m_drawBuffer.width() < bufwid || m_drawBuffer.height() < h) {
            m_drawBuffer = QImage(bufwid, h, QImage::Format_Indexed8);
        }
        usePeaksCache = (increment * 8) < zoomLevel;
        if (m_colourScale == PhaseColourScale) usePeaksCache = false;
    }

    m_drawBuffer.setNumColors(256);
    for (int pixel = 0; pixel < 256; ++pixel) {
        m_drawBuffer.setColor(pixel, m_palette.getColour(pixel).rgb());
    }

    m_drawBuffer.fill(0);
    
    if (m_binDisplay != PeakFrequencies) {

        for (int y = 0; y < h; ++y) {
            float q0 = 0, q1 = 0;
            if (!getSmoothedYBinRange(v, h-y-1, q0, q1)) {
                binfory[y] = -1;
            } else {
                binfory[y] = q0;
//                cerr << "binfory[" << y << "] = " << binfory[y] << endl;
            }
        }

        paintDrawBuffer(v, bufwid, h, binforx, binfory, usePeaksCache,
                        overallMag, overallMagChanged);

    } else {

        paintDrawBufferPeakFrequencies(v, bufwid, h, binforx,
                                       minbin, maxbin,
                                       displayMinFreq, displayMaxFreq,
                                       logarithmic,
                                       overallMag, overallMagChanged);
    }

/*
    for (int x = 0; x < w / xPixelRatio; ++x) {

        Profiler innerprof("SpectrogramLayer::paint: 1 pixel column");

        runOutOfData = !paintColumnValues(v, fft, x0, x,
                                          minbin, maxbin,
                                          displayMinFreq, displayMaxFreq,
                                          xPixelRatio,
                                          h, yforbin);

        if (runOutOfData) {
#ifdef DEBUG_SPECTROGRAM_REPAINT
            std::cerr << "Run out of data -- dropping out of loop" << std::endl;
#endif
            break;
        }
    }
*/
#ifdef DEBUG_SPECTROGRAM_REPAINT
//    std::cerr << pixels << " pixels drawn" << std::endl;
#endif

    if (overallMagChanged) {
        m_viewMags[v] = overallMag;
#ifdef DEBUG_SPECTROGRAM_REPAINT
        std::cerr << "Overall mag is now [" << m_viewMags[v].getMin() << "->" << m_viewMags[v].getMax() << "] - will be updating" << std::endl;
#endif
    } else {
#ifdef DEBUG_SPECTROGRAM_REPAINT
        std::cerr << "Overall mag unchanged at [" << m_viewMags[v].getMin() << "->" << m_viewMags[v].getMax() << "]" << std::endl;
#endif
    }

    outerprof.end();

    Profiler profiler2("SpectrogramLayer::paint: draw image");

    if (recreateWholeImageCache) {
#ifdef DEBUG_SPECTROGRAM_REPAINT
        std::cerr << "Recreating image cache: width = " << v->width()
                  << ", height = " << h << std::endl;
#endif
	cache.image = QImage(v->width(), h, QImage::Format_ARGB32_Premultiplied);
    }

    if (w > 0) {
#ifdef DEBUG_SPECTROGRAM_REPAINT
        std::cerr << "Painting " << w << "x" << h
                  << " from draw buffer at " << 0 << "," << 0
                  << " to " << w << "x" << h << " on cache at "
                  << x0 << "," << 0 << std::endl;
#endif

        QPainter cachePainter(&cache.image);

        if (bufferBinResolution) {
            int scaledLeft = v->getXForFrame(leftBoundaryFrame);
            int scaledRight = v->getXForFrame(rightBoundaryFrame);
#ifdef DEBUG_SPECTROGRAM_REPAINT
            cerr << "Rescaling image from " << bufwid
                 << "x" << h << " to "
                 << scaledRight-scaledLeft << "x" << h << endl;
#endif
            Preferences::SpectrogramXSmoothing xsmoothing = 
                Preferences::getInstance()->getSpectrogramXSmoothing();
//            cerr << "xsmoothing == " << xsmoothing << endl;
            QImage scaled = m_drawBuffer.scaled
                (scaledRight - scaledLeft, h,
                 Qt::IgnoreAspectRatio,
                 ((xsmoothing == Preferences::SpectrogramXInterpolated) ?
                  Qt::SmoothTransformation : Qt::FastTransformation));
            int scaledLeftCrop = v->getXForFrame(leftCropFrame);
            int scaledRightCrop = v->getXForFrame(rightCropFrame);
#ifdef DEBUG_SPECTROGRAM_REPAINT
            cerr << "Drawing image region of width " << scaledRightCrop - scaledLeftCrop << " to "
                 << scaledLeftCrop << " from " << scaledLeftCrop - scaledLeft << endl;
#endif
            cachePainter.drawImage
                (QRect(scaledLeftCrop, 0,
                       scaledRightCrop - scaledLeftCrop, h),
                 scaled,
                 QRect(scaledLeftCrop - scaledLeft, 0,
                       scaledRightCrop - scaledLeftCrop, h));
        } else {
            cachePainter.drawImage(QRect(x0, 0, w, h),
                                   m_drawBuffer,
                                   QRect(0, 0, w, h));
        }

        cachePainter.end();
    }

    QRect pr = rect & cache.validArea;

#ifdef DEBUG_SPECTROGRAM_REPAINT
    std::cerr << "Painting " << pr.width() << "x" << pr.height()
              << " from cache at " << pr.x() << "," << pr.y()
              << " to window" << std::endl;
#endif

    paint.drawImage(pr.x(), pr.y(), cache.image,
                    pr.x(), pr.y(), pr.width(), pr.height());
    //!!!
//    paint.drawImage(v->rect(), cache.image,
//                    QRect(QPoint(0, 0), cache.image.size()));

    cache.startFrame = startFrame;
    cache.zoomLevel = zoomLevel;

    if (!m_synchronous) {

        if (!m_normalizeVisibleArea || !overallMagChanged) {
    
            if (cache.validArea.x() > 0) {
#ifdef DEBUG_SPECTROGRAM_REPAINT
                std::cerr << "SpectrogramLayer::paint() updating left (0, "
                          << cache.validArea.x() << ")" << std::endl;
#endif
                v->update(0, 0, cache.validArea.x(), h);
            }
            
            if (cache.validArea.x() + cache.validArea.width() <
                cache.image.width()) {
#ifdef DEBUG_SPECTROGRAM_REPAINT
                std::cerr << "SpectrogramLayer::paint() updating right ("
                          << cache.validArea.x() + cache.validArea.width()
                          << ", "
                          << cache.image.width() - (cache.validArea.x() +
                                                     cache.validArea.width())
                          << ")" << std::endl;
#endif
                v->update(cache.validArea.x() + cache.validArea.width(),
                          0,
                          cache.image.width() - (cache.validArea.x() +
                                                  cache.validArea.width()),
                          h);
            }
        } else {
            // overallMagChanged
            std::cerr << "\noverallMagChanged - updating all\n" << std::endl;
            cache.validArea = QRect();
            v->update();
        }
    }

    illuminateLocalFeatures(v, paint);

#ifdef DEBUG_SPECTROGRAM_REPAINT
    std::cerr << "SpectrogramLayer::paint() returning" << std::endl;
#endif

    if (!m_synchronous) {
        m_lastPaintBlockWidth = paintBlockWidth;
        (void)gettimeofday(&tv, 0);
        m_lastPaintTime = RealTime::fromTimeval(tv) - mainPaintStart;
    }

//!!!    if (fftSuspended) fft->resume();
}

bool
SpectrogramLayer::paintDrawBufferPeakFrequencies(View *v,
                                                 int w,
                                                 int h,
                                                 int *binforx,
                                                 int minbin,
                                                 int maxbin,
                                                 float displayMinFreq,
                                                 float displayMaxFreq,
                                                 bool logarithmic,
                                                 MagnitudeRange &overallMag,
                                                 bool &overallMagChanged) const
{
    Profiler profiler("SpectrogramLayer::paintDrawBufferPeakFrequencies");

#ifdef DEBUG_SPECTROGRAM_REPAINT
    cerr << "minbin " << minbin << ", maxbin " << maxbin << "; w " << w << ", h " << h << endl;
#endif
    if (minbin < 0) minbin = 0;
    if (maxbin < 0) maxbin = minbin+1;

    FFTModel *fft = getFFTModel(v);
    if (!fft) return false;

    FFTModel::PeakSet peakfreqs;

    int px = -1, psx = -1;

#ifdef __GNUC__
    float values[maxbin - minbin + 1];
#else
    float *values = (float *)alloca((maxbin - minbin + 1) * sizeof(float));
#endif

    for (int x = 0; x < w; ++x) {
        
        if (binforx[x] < 0) continue;

        float columnMax = 0.f;

        int sx0 = binforx[x];
        int sx1 = sx0;
        if (x+1 < w) sx1 = binforx[x+1];
        if (sx0 < 0) sx0 = sx1 - 1;
        if (sx0 < 0) continue;
        if (sx1 <= sx0) sx1 = sx0 + 1;

        for (int sx = sx0; sx < sx1; ++sx) {

            if (x == px && sx == psx) continue;
            if (sx < 0 || sx >= int(fft->getWidth())) continue;

            if (!m_synchronous) {
                if (!fft->isColumnAvailable(sx)) {
#ifdef DEBUG_SPECTROGRAM_REPAINT
                    std::cerr << "Met unavailable column at col " << sx << std::endl;
#endif
                    return false;
                }
            }

            MagnitudeRange mag;

            if (sx != psx) {
                peakfreqs = fft->getPeakFrequencies(FFTModel::AllPeaks, sx,
                                                    minbin, maxbin - 1);
                if (m_colourScale == PhaseColourScale) {
                    fft->getPhasesAt(sx, values, minbin, maxbin - minbin + 1);
                } else if (m_normalizeColumns) {
                    fft->getNormalizedMagnitudesAt(sx, values, minbin, maxbin - minbin + 1);
                } else {
                    fft->getMagnitudesAt(sx, values, minbin, maxbin - minbin + 1);
                }
                psx = sx;
            }

            for (FFTModel::PeakSet::const_iterator pi = peakfreqs.begin();
                 pi != peakfreqs.end(); ++pi) {

                int bin = pi->first;
                int freq = pi->second;

                if (bin < minbin) continue;
                if (bin > maxbin) break;

                float value = values[bin - minbin];

                if (m_colourScale != PhaseColourScale) {
                    if (!m_normalizeColumns) {
                        value /= (m_fftSize/2.f);
                    }
                    mag.sample(value);
                    value *= m_gain;
                }

                float y = v->getYForFrequency
                    (freq, displayMinFreq, displayMaxFreq, logarithmic);

                int iy = int(y + 0.5);
                if (iy < 0 || iy >= h) continue;

                m_drawBuffer.setPixel(x, iy, getDisplayValue(v, value));
            }

            if (mag.isSet()) {
                if (sx >= int(m_columnMags.size())) {
#ifdef DEBUG_SPECTROGRAM
                    std::cerr << "INTERNAL ERROR: " << sx << " >= "
                              << m_columnMags.size()
                              << " at SpectrogramLayer.cpp::paintDrawBuffer"
                              << std::endl;
#endif
                } else {
                    m_columnMags[sx].sample(mag);
                    if (overallMag.sample(mag)) overallMagChanged = true;
                }
            }
        }
    }

    return true;
}

bool
SpectrogramLayer::paintDrawBuffer(View *v,
                                  int w,
                                  int h,
                                  int *binforx,
                                  float *binfory,
                                  bool usePeaksCache,
                                  MagnitudeRange &overallMag,
                                  bool &overallMagChanged) const
{
    Profiler profiler("SpectrogramLayer::paintDrawBuffer");

    int minbin = int(binfory[0] + 0.0001);
    int maxbin = binfory[h-1];

#ifdef DEBUG_SPECTROGRAM_REPAINT
    cerr << "minbin " << minbin << ", maxbin " << maxbin << "; w " << w << ", h " << h << endl;
#endif
    if (minbin < 0) minbin = 0;
    if (maxbin < 0) maxbin = minbin+1;

    DenseThreeDimensionalModel *sourceModel = 0;
    FFTModel *fft = 0;
    int divisor = 1;
#ifdef DEBUG_SPECTROGRAM_REPAINT
    cerr << "Note: bin display = " << m_binDisplay << ", w = " << w << ", binforx[" << w-1 << "] = " << binforx[w-1] << ", binforx[0] = " << binforx[0] << endl;
#endif
    if (usePeaksCache) { //!!!
        sourceModel = getPeakCache(v);
        divisor = 8;//!!!
        minbin = 0;
        maxbin = sourceModel->getHeight();
    } else {
        sourceModel = fft = getFFTModel(v);
    }

    if (!sourceModel) return false;

    bool interpolate = false;
    Preferences::SpectrogramSmoothing smoothing = 
        Preferences::getInstance()->getSpectrogramSmoothing();
    if (smoothing == Preferences::SpectrogramInterpolated ||
        smoothing == Preferences::SpectrogramZeroPaddedAndInterpolated) {
        if (m_binDisplay != PeakBins &&
            m_binDisplay != PeakFrequencies) {
            interpolate = true;
        }
    }

    int psx = -1;

#ifdef __GNUC__
    float autoarray[maxbin - minbin + 1];
    float peaks[h];
#else
    float *autoarray = (float *)alloca((maxbin - minbin + 1) * sizeof(float));
    float *peaks = (float *)alloca(h * sizeof(float));
#endif

    const float *values = autoarray;
    DenseThreeDimensionalModel::Column c;

    for (int x = 0; x < w; ++x) {
        
        if (binforx[x] < 0) continue;

//        float columnGain = m_gain;
        float columnMax = 0.f;

        int sx0 = binforx[x] / divisor;
        int sx1 = sx0;
        if (x+1 < w) sx1 = binforx[x+1] / divisor;
        if (sx0 < 0) sx0 = sx1 - 1;
        if (sx0 < 0) continue;
        if (sx1 <= sx0) sx1 = sx0 + 1;

        for (int y = 0; y < h; ++y) peaks[y] = 0.f;
            
        for (int sx = sx0; sx < sx1; ++sx) {

#ifdef DEBUG_SPECTROGRAM_REPAINT
//            std::cerr << "sx = " << sx << std::endl;
#endif

            if (sx < 0 || sx >= int(sourceModel->getWidth())) continue;

            if (!m_synchronous) {
                if (!sourceModel->isColumnAvailable(sx)) {
#ifdef DEBUG_SPECTROGRAM_REPAINT
                    std::cerr << "Met unavailable column at col " << sx << std::endl;
#endif
                    return false;
                }
            }

            MagnitudeRange mag;

            if (sx != psx) {
                if (fft) {
#ifdef DEBUG_SPECTROGRAM_REPAINT
                    cerr << "Retrieving column " << sx << " from fft directly" << endl;
#endif
                    if (m_colourScale == PhaseColourScale) {
                        fft->getPhasesAt(sx, autoarray, minbin, maxbin - minbin + 1);
                    } else if (m_normalizeColumns) {
                        fft->getNormalizedMagnitudesAt(sx, autoarray, minbin, maxbin - minbin + 1);
                    } else {
                        fft->getMagnitudesAt(sx, autoarray, minbin, maxbin - minbin + 1);
                    }
                } else {
#ifdef DEBUG_SPECTROGRAM_REPAINT
                    cerr << "Retrieving column " << sx << " from peaks cache" << endl;
#endif
                    c = sourceModel->getColumn(sx);
                    if (m_normalizeColumns) {
                        for (int y = 0; y < h; ++y) {
                            if (c[y] > columnMax) columnMax = c[y];
                        }
                    }
                    values = c.constData() + minbin;
                }
                psx = sx;
            }

            for (int y = 0; y < h; ++y) {

                float sy0 = binfory[y];
                float sy1 = sy0 + 1;
                if (y+1 < h) sy1 = binfory[y+1];

                float value = 0.f;

                if (interpolate && fabsf(sy1 - sy0) < 1.f) {

                    float centre = (sy0 + sy1) / 2;
                    float dist = (centre - 0.5) - lrintf(centre - 0.5);
                    int bin = int(centre);
                    int other = (dist < 0 ? (bin-1) : (bin+1));
                    if (bin < minbin) bin = minbin;
                    if (bin > maxbin) bin = maxbin;
                    if (other < minbin || other > maxbin) other = bin;
                    float prop = 1.f - fabsf(dist);

                    float v0 = values[bin - minbin];
                    float v1 = values[other - minbin];
                    if (m_binDisplay == PeakBins) {
                        if (bin == minbin || bin == maxbin ||
                            v0 < values[bin-minbin-1] ||
                            v0 < values[bin-minbin+1]) v0 = 0.f;
                        if (other == minbin || other == maxbin ||
                            v1 < values[other-minbin-1] ||
                            v1 < values[other-minbin+1]) v1 = 0.f;
                    }
                    if (v0 == 0.f && v1 == 0.f) continue;
                    value = prop * v0 + (1.f - prop) * v1;

                    if (m_colourScale != PhaseColourScale) {
                        if (!m_normalizeColumns) {
                            value /= (m_fftSize/2.f);
                        }
                        mag.sample(value);
                        value *= m_gain;
                    }

                    peaks[y] = value;

                } else {                    

                    int by0 = int(sy0 + 0.0001);
                    int by1 = int(sy1 + 0.0001);
                    if (by1 < by0 + 1) by1 = by0 + 1;

                    for (int bin = by0; bin < by1; ++bin) {

                        value = values[bin - minbin];
                        if (m_binDisplay == PeakBins) {
                            if (bin == minbin || bin == maxbin ||
                                value < values[bin-minbin-1] ||
                                value < values[bin-minbin+1]) continue;
                        }

                        if (m_colourScale != PhaseColourScale) {
                            if (!m_normalizeColumns) {
                                value /= (m_fftSize/2.f);
                            }
                            mag.sample(value);
                            value *= m_gain;
                        }

                        if (value > peaks[y]) peaks[y] = value; //!!! not right for phase!
                    }
                }
            }

            if (mag.isSet()) {
                if (sx >= int(m_columnMags.size())) {
#ifdef DEBUG_SPECTROGRAM
                    std::cerr << "INTERNAL ERROR: " << sx << " >= "
                              << m_columnMags.size()
                              << " at SpectrogramLayer.cpp::paintDrawBuffer"
                              << std::endl;
#endif
                } else {
                    m_columnMags[sx].sample(mag);
                    if (overallMag.sample(mag)) overallMagChanged = true;
                }
            }
        }

        for (int y = 0; y < h; ++y) {

            float peak = peaks[y];
            
            if (m_colourScale != PhaseColourScale &&
                m_normalizeColumns && 
                columnMax > 0.f) {
                peak /= columnMax;
            }
            
            unsigned char peakpix = getDisplayValue(v, peak);

            m_drawBuffer.setPixel(x, h-y-1, peakpix);
        }
    }

    return true;
}

void
SpectrogramLayer::illuminateLocalFeatures(View *v, QPainter &paint) const
{
    Profiler profiler("SpectrogramLayer::illuminateLocalFeatures");

    QPoint localPos;
    if (!v->shouldIlluminateLocalFeatures(this, localPos) || !m_model) {
        return;
    }

//    std::cerr << "SpectrogramLayer: illuminateLocalFeatures("
//              << localPos.x() << "," << localPos.y() << ")" << std::endl;

    float s0, s1;
    float f0, f1;

    if (getXBinRange(v, localPos.x(), s0, s1) &&
        getYBinSourceRange(v, localPos.y(), f0, f1)) {
        
        int s0i = int(s0 + 0.001);
        int s1i = int(s1);
        
        int x0 = v->getXForFrame(s0i * getWindowIncrement());
        int x1 = v->getXForFrame((s1i + 1) * getWindowIncrement());

        int y1 = int(getYForFrequency(v, f1));
        int y0 = int(getYForFrequency(v, f0));
        
//        std::cerr << "SpectrogramLayer: illuminate "
//                  << x0 << "," << y1 << " -> " << x1 << "," << y0 << std::endl;
        
        paint.setPen(v->getForeground());

        //!!! should we be using paintCrosshairs for this?

        paint.drawRect(x0, y1, x1 - x0 + 1, y0 - y1 + 1);
    }
}

float
SpectrogramLayer::getYForFrequency(const View *v, float frequency) const
{
    return v->getYForFrequency(frequency,
			       getEffectiveMinFrequency(),
			       getEffectiveMaxFrequency(),
			       m_frequencyScale == LogFrequencyScale);
}

float
SpectrogramLayer::getFrequencyForY(const View *v, int y) const
{
    return v->getFrequencyForY(y,
			       getEffectiveMinFrequency(),
			       getEffectiveMaxFrequency(),
			       m_frequencyScale == LogFrequencyScale);
}

int
SpectrogramLayer::getCompletion(View *v) const
{
    if (m_updateTimer == 0) return 100;
    if (m_fftModels.find(v) == m_fftModels.end()) return 100;

    size_t completion = m_fftModels[v].first->getCompletion();
#ifdef DEBUG_SPECTROGRAM_REPAINT
    std::cerr << "SpectrogramLayer::getCompletion: completion = " << completion << std::endl;
#endif
    return completion;
}

bool
SpectrogramLayer::getValueExtents(float &min, float &max,
                                  bool &logarithmic, QString &unit) const
{
    if (!m_model) return false;

    int sr = m_model->getSampleRate();
    min = float(sr) / m_fftSize;
    max = float(sr) / 2;
    
    logarithmic = (m_frequencyScale == LogFrequencyScale);
    unit = "Hz";
    return true;
}

bool
SpectrogramLayer::getDisplayExtents(float &min, float &max) const
{
    min = getEffectiveMinFrequency();
    max = getEffectiveMaxFrequency();

//    std::cerr << "SpectrogramLayer::getDisplayExtents: " << min << "->" << max << std::endl;
    return true;
}    

bool
SpectrogramLayer::setDisplayExtents(float min, float max)
{
    if (!m_model) return false;

//    std::cerr << "SpectrogramLayer::setDisplayExtents: " << min << "->" << max << std::endl;

    if (min < 0) min = 0;
    if (max > m_model->getSampleRate()/2) max = m_model->getSampleRate()/2;
    
    size_t minf = lrintf(min);
    size_t maxf = lrintf(max);

    if (m_minFrequency == minf && m_maxFrequency == maxf) return true;

    invalidateImageCaches();
    invalidateMagnitudes();

    m_minFrequency = minf;
    m_maxFrequency = maxf;
    
    emit layerParametersChanged();

    int vs = getCurrentVerticalZoomStep();
    if (vs != m_lastEmittedZoomStep) {
        emit verticalZoomChanged();
        m_lastEmittedZoomStep = vs;
    }

    return true;
}

bool
SpectrogramLayer::getYScaleValue(const View *v, int y,
                                 float &value, QString &unit) const
{
    value = getFrequencyForY(v, y);
    unit = "Hz";
    return true;
}

bool
SpectrogramLayer::snapToFeatureFrame(View *, int &frame,
				     size_t &resolution,
				     SnapType snap) const
{
    resolution = getWindowIncrement();
    int left = (frame / resolution) * resolution;
    int right = left + resolution;

    switch (snap) {
    case SnapLeft:  frame = left;  break;
    case SnapRight: frame = right; break;
    case SnapNearest:
    case SnapNeighbouring:
	if (frame - left > right - frame) frame = right;
	else frame = left;
	break;
    }
    
    return true;
} 

void
SpectrogramLayer::measureDoubleClick(View *v, QMouseEvent *e)
{
    ImageCache &cache = m_imageCaches[v];

    std::cerr << "cache width: " << cache.image.width() << ", height: "
              << cache.image.height() << std::endl;

    QImage image = cache.image;

    ImageRegionFinder finder;
    QRect rect = finder.findRegionExtents(&image, e->pos());
    if (rect.isValid()) {
        MeasureRect mr;
        setMeasureRectFromPixrect(v, mr, rect);
        CommandHistory::getInstance()->addCommand
            (new AddMeasurementRectCommand(this, mr));
    }
}

bool
SpectrogramLayer::getCrosshairExtents(View *v, QPainter &paint,
                                      QPoint cursorPos,
                                      std::vector<QRect> &extents) const
{
    QRect vertical(cursorPos.x() - 12, 0, 12, v->height());
    extents.push_back(vertical);

    QRect horizontal(0, cursorPos.y(), cursorPos.x(), 1);
    extents.push_back(horizontal);

    int sw = getVerticalScaleWidth(v, paint);

    QRect freq(sw, cursorPos.y() - paint.fontMetrics().ascent() - 2,
               paint.fontMetrics().width("123456 Hz") + 2,
               paint.fontMetrics().height());
    extents.push_back(freq);

    QRect pitch(sw, cursorPos.y() + 2,
                paint.fontMetrics().width("C#10+50c") + 2,
                paint.fontMetrics().height());
    extents.push_back(pitch);

    QRect rt(cursorPos.x(),
             v->height() - paint.fontMetrics().height() - 2,
             paint.fontMetrics().width("1234.567 s"),
             paint.fontMetrics().height());
    extents.push_back(rt);

    int w(paint.fontMetrics().width("1234567890") + 2);
    QRect frame(cursorPos.x() - w - 2,
                v->height() - paint.fontMetrics().height() - 2,
                w,
                paint.fontMetrics().height());
    extents.push_back(frame);

    return true;
}

void
SpectrogramLayer::paintCrosshairs(View *v, QPainter &paint,
                                  QPoint cursorPos) const
{
    paint.save();

    int sw = getVerticalScaleWidth(v, paint);

    QFont fn = paint.font();
    if (fn.pointSize() > 8) {
        fn.setPointSize(fn.pointSize() - 1);
        paint.setFont(fn);
    }
    paint.setPen(m_crosshairColour);

    paint.drawLine(0, cursorPos.y(), cursorPos.x() - 1, cursorPos.y());
    paint.drawLine(cursorPos.x(), 0, cursorPos.x(), v->height());
    
    float fundamental = getFrequencyForY(v, cursorPos.y());

    v->drawVisibleText(paint,
                       sw + 2,
                       cursorPos.y() - 2,
                       QString("%1 Hz").arg(fundamental),
                       View::OutlinedText);

    if (Pitch::isFrequencyInMidiRange(fundamental)) {
        QString pitchLabel = Pitch::getPitchLabelForFrequency(fundamental);
        v->drawVisibleText(paint,
                           sw + 2,
                           cursorPos.y() + paint.fontMetrics().ascent() + 2,
                           pitchLabel,
                           View::OutlinedText);
    }

    long frame = v->getFrameForX(cursorPos.x());
    RealTime rt = RealTime::frame2RealTime(frame, m_model->getSampleRate());
    QString rtLabel = QString("%1 s").arg(rt.toText(true).c_str());
    QString frameLabel = QString("%1").arg(frame);
    v->drawVisibleText(paint,
                       cursorPos.x() - paint.fontMetrics().width(frameLabel) - 2,
                       v->height() - 2,
                       frameLabel,
                       View::OutlinedText);
    v->drawVisibleText(paint,
                       cursorPos.x() + 2,
                       v->height() - 2,
                       rtLabel,
                       View::OutlinedText);

    int harmonic = 2;

    while (harmonic < 100) {

        float hy = lrintf(getYForFrequency(v, fundamental * harmonic));
        if (hy < 0 || hy > v->height()) break;
        
        int len = 7;

        if (harmonic % 2 == 0) {
            if (harmonic % 4 == 0) {
                len = 12;
            } else {
                len = 10;
            }
        }

        paint.drawLine(cursorPos.x() - len,
                       int(hy),
                       cursorPos.x(),
                       int(hy));

        ++harmonic;
    }

    paint.restore();
}

QString
SpectrogramLayer::getFeatureDescription(View *v, QPoint &pos) const
{
    int x = pos.x();
    int y = pos.y();

    if (!m_model || !m_model->isOK()) return "";

    float magMin = 0, magMax = 0;
    float phaseMin = 0, phaseMax = 0;
    float freqMin = 0, freqMax = 0;
    float adjFreqMin = 0, adjFreqMax = 0;
    QString pitchMin, pitchMax;
    RealTime rtMin, rtMax;

    bool haveValues = false;

    if (!getXBinSourceRange(v, x, rtMin, rtMax)) {
	return "";
    }
    if (getXYBinSourceRange(v, x, y, magMin, magMax, phaseMin, phaseMax)) {
	haveValues = true;
    }

    QString adjFreqText = "", adjPitchText = "";

    if (m_binDisplay == PeakFrequencies) {

	if (!getAdjustedYBinSourceRange(v, x, y, freqMin, freqMax,
					adjFreqMin, adjFreqMax)) {
	    return "";
	}

	if (adjFreqMin != adjFreqMax) {
	    adjFreqText = tr("Peak Frequency:\t%1 - %2 Hz\n")
		.arg(adjFreqMin).arg(adjFreqMax);
	} else {
	    adjFreqText = tr("Peak Frequency:\t%1 Hz\n")
		.arg(adjFreqMin);
	}

	QString pmin = Pitch::getPitchLabelForFrequency(adjFreqMin);
	QString pmax = Pitch::getPitchLabelForFrequency(adjFreqMax);

	if (pmin != pmax) {
	    adjPitchText = tr("Peak Pitch:\t%3 - %4\n").arg(pmin).arg(pmax);
	} else {
	    adjPitchText = tr("Peak Pitch:\t%2\n").arg(pmin);
	}

    } else {
	
	if (!getYBinSourceRange(v, y, freqMin, freqMax)) return "";
    }

    QString text;

    if (rtMin != rtMax) {
	text += tr("Time:\t%1 - %2\n")
	    .arg(rtMin.toText(true).c_str())
	    .arg(rtMax.toText(true).c_str());
    } else {
	text += tr("Time:\t%1\n")
	    .arg(rtMin.toText(true).c_str());
    }

    if (freqMin != freqMax) {
	text += tr("%1Bin Frequency:\t%2 - %3 Hz\n%4Bin Pitch:\t%5 - %6\n")
	    .arg(adjFreqText)
	    .arg(freqMin)
	    .arg(freqMax)
	    .arg(adjPitchText)
	    .arg(Pitch::getPitchLabelForFrequency(freqMin))
	    .arg(Pitch::getPitchLabelForFrequency(freqMax));
    } else {
	text += tr("%1Bin Frequency:\t%2 Hz\n%3Bin Pitch:\t%4\n")
	    .arg(adjFreqText)
	    .arg(freqMin)
	    .arg(adjPitchText)
	    .arg(Pitch::getPitchLabelForFrequency(freqMin));
    }	

    if (haveValues) {
	float dbMin = AudioLevel::multiplier_to_dB(magMin);
	float dbMax = AudioLevel::multiplier_to_dB(magMax);
	QString dbMinString;
	QString dbMaxString;
	if (dbMin == AudioLevel::DB_FLOOR) {
	    dbMinString = tr("-Inf");
	} else {
	    dbMinString = QString("%1").arg(lrintf(dbMin));
	}
	if (dbMax == AudioLevel::DB_FLOOR) {
	    dbMaxString = tr("-Inf");
	} else {
	    dbMaxString = QString("%1").arg(lrintf(dbMax));
	}
	if (lrintf(dbMin) != lrintf(dbMax)) {
	    text += tr("dB:\t%1 - %2").arg(dbMinString).arg(dbMaxString);
	} else {
	    text += tr("dB:\t%1").arg(dbMinString);
	}
	if (phaseMin != phaseMax) {
	    text += tr("\nPhase:\t%1 - %2").arg(phaseMin).arg(phaseMax);
	} else {
	    text += tr("\nPhase:\t%1").arg(phaseMin);
	}
    }

    return text;
}

int
SpectrogramLayer::getColourScaleWidth(QPainter &paint) const
{
    int cw;

    cw = paint.fontMetrics().width("-80dB");

    return cw;
}

int
SpectrogramLayer::getVerticalScaleWidth(View *, QPainter &paint) const
{
    if (!m_model || !m_model->isOK()) return 0;

    int cw = getColourScaleWidth(paint);

    int tw = paint.fontMetrics().width(QString("%1")
				     .arg(m_maxFrequency > 0 ?
					  m_maxFrequency - 1 :
					  m_model->getSampleRate() / 2));

    int fw = paint.fontMetrics().width(tr("43Hz"));
    if (tw < fw) tw = fw;

    int tickw = (m_frequencyScale == LogFrequencyScale ? 10 : 4);
    
    return cw + tickw + tw + 13;
}

void
SpectrogramLayer::paintVerticalScale(View *v, QPainter &paint, QRect rect) const
{
    if (!m_model || !m_model->isOK()) {
	return;
    }

    Profiler profiler("SpectrogramLayer::paintVerticalScale");

    //!!! cache this?

    int h = rect.height(), w = rect.width();

    int tickw = (m_frequencyScale == LogFrequencyScale ? 10 : 4);
    int pkw = (m_frequencyScale == LogFrequencyScale ? 10 : 0);

    size_t bins = m_fftSize / 2;
    int sr = m_model->getSampleRate();

    if (m_maxFrequency > 0) {
	bins = int((double(m_maxFrequency) * m_fftSize) / sr + 0.1);
	if (bins > m_fftSize / 2) bins = m_fftSize / 2;
    }

    int cw = getColourScaleWidth(paint);
    int cbw = paint.fontMetrics().width("dB");

    int py = -1;
    int textHeight = paint.fontMetrics().height();
    int toff = -textHeight + paint.fontMetrics().ascent() + 2;

    if (h > textHeight * 3 + 10) {

        int topLines = 2;
        if (m_colourScale == PhaseColourScale) topLines = 1;

	int ch = h - textHeight * (topLines + 1) - 8;
//	paint.drawRect(4, textHeight + 4, cw - 1, ch + 1);
	paint.drawRect(4 + cw - cbw, textHeight * topLines + 4, cbw - 1, ch + 1);

	QString top, bottom;
        float min = m_viewMags[v].getMin();
        float max = m_viewMags[v].getMax();

        float dBmin = AudioLevel::multiplier_to_dB(min);
        float dBmax = AudioLevel::multiplier_to_dB(max);

        if (dBmax < -60.f) dBmax = -60.f;
        else top = QString("%1").arg(lrintf(dBmax));

        if (dBmin < dBmax - 60.f) dBmin = dBmax - 60.f;
        bottom = QString("%1").arg(lrintf(dBmin));

        //!!! & phase etc

        if (m_colourScale != PhaseColourScale) {
            paint.drawText((cw + 6 - paint.fontMetrics().width("dBFS")) / 2,
                           2 + textHeight + toff, "dBFS");
        }

//	paint.drawText((cw + 6 - paint.fontMetrics().width(top)) / 2,
	paint.drawText(3 + cw - cbw - paint.fontMetrics().width(top),
		       2 + textHeight * topLines + toff + textHeight/2, top);

	paint.drawText(3 + cw - cbw - paint.fontMetrics().width(bottom),
		       h + toff - 3 - textHeight/2, bottom);

	paint.save();
	paint.setBrush(Qt::NoBrush);

        int lasty = 0;
        int lastdb = 0;

	for (int i = 0; i < ch; ++i) {

            float dBval = dBmin + (((dBmax - dBmin) * i) / (ch - 1));
            int idb = int(dBval);

            float value = AudioLevel::dB_to_multiplier(dBval);
            int colour = getDisplayValue(v, value * m_gain);

	    paint.setPen(m_palette.getColour(colour));

            int y = textHeight * topLines + 4 + ch - i;

            paint.drawLine(5 + cw - cbw, y, cw + 2, y);

            if (i == 0) {
                lasty = y;
                lastdb = idb;
            } else if (i < ch - paint.fontMetrics().ascent() &&
                       idb != lastdb &&
                       ((abs(y - lasty) > textHeight && 
                         idb % 10 == 0) ||
                        (abs(y - lasty) > paint.fontMetrics().ascent() && 
                         idb % 5 == 0))) {
                paint.setPen(v->getBackground());
                QString text = QString("%1").arg(idb);
                paint.drawText(3 + cw - cbw - paint.fontMetrics().width(text),
                               y + toff + textHeight/2, text);
                paint.setPen(v->getForeground());
                paint.drawLine(5 + cw - cbw, y, 8 + cw - cbw, y);
                lasty = y;
                lastdb = idb;
            }
	}
	paint.restore();
    }

    paint.drawLine(cw + 7, 0, cw + 7, h);

    int bin = -1;

    for (int y = 0; y < v->height(); ++y) {

	float q0, q1;
	if (!getYBinRange(v, v->height() - y, q0, q1)) continue;

	int vy;

	if (int(q0) > bin) {
	    vy = y;
	    bin = int(q0);
	} else {
	    continue;
	}

	int freq = (sr * bin) / m_fftSize;

	if (py >= 0 && (vy - py) < textHeight - 1) {
	    if (m_frequencyScale == LinearFrequencyScale) {
		paint.drawLine(w - tickw, h - vy, w, h - vy);
	    }
	    continue;
	}

	QString text = QString("%1").arg(freq);
	if (bin == 1) text = tr("%1Hz").arg(freq); // bin 0 is DC
	paint.drawLine(cw + 7, h - vy, w - pkw - 1, h - vy);

	if (h - vy - textHeight >= -2) {
	    int tx = w - 3 - paint.fontMetrics().width(text) - std::max(tickw, pkw);
	    paint.drawText(tx, h - vy + toff, text);
	}

	py = vy;
    }

    if (m_frequencyScale == LogFrequencyScale) {

        // piano keyboard

	paint.drawLine(w - pkw - 1, 0, w - pkw - 1, h);

	float minf = getEffectiveMinFrequency();
	float maxf = getEffectiveMaxFrequency();

	int py = h, ppy = h;
	paint.setBrush(paint.pen().color());

	for (int i = 0; i < 128; ++i) {

	    float f = Pitch::getFrequencyForPitch(i);
	    int y = lrintf(v->getYForFrequency(f, minf, maxf, true));

            if (y < -2) break;
            if (y > h + 2) {
                continue;
            }

	    int n = (i % 12);

            if (n == 1) {
                // C# -- fill the C from here
                QColor col = Qt::gray;
                if (i == 61) { // filling middle C
                    col = Qt::blue;
                    col = col.light(150);
                }
                if (ppy - y > 2) {
                    paint.fillRect(w - pkw,
                                   y,
                                   pkw,
                                   (py + ppy) / 2 - y,
                                   col);
                }
            }

	    if (n == 1 || n == 3 || n == 6 || n == 8 || n == 10) {
		// black notes
		paint.drawLine(w - pkw, y, w, y);
		int rh = ((py - y) / 4) * 2;
		if (rh < 2) rh = 2;
		paint.drawRect(w - pkw, y - (py-y)/4, pkw/2, rh);
	    } else if (n == 0 || n == 5) {
		// C, F
		if (py < h) {
		    paint.drawLine(w - pkw, (y + py) / 2, w, (y + py) / 2);
		}
	    }

            ppy = py;
	    py = y;
	}
    }
}

class SpectrogramRangeMapper : public RangeMapper
{
public:
    SpectrogramRangeMapper(int sr, int /* fftsize */) :
        m_dist(float(sr) / 2),
        m_s2(sqrtf(sqrtf(2))) { }
    ~SpectrogramRangeMapper() { }
    
    virtual int getPositionForValue(float value) const {

        float dist = m_dist;
    
        int n = 0;

        while (dist > (value + 0.00001) && dist > 0.1f) {
            dist /= m_s2;
            ++n;
        }

        return n;
    }

    virtual float getValueForPosition(int position) const {

        // Vertical zoom step 0 shows the entire range from DC ->
        // Nyquist frequency.  Step 1 shows 2^(1/4) of the range of
        // step 0, and so on until the visible range is smaller than
        // the frequency step between bins at the current fft size.

        float dist = m_dist;
    
        int n = 0;
        while (n < position) {
            dist /= m_s2;
            ++n;
        }

        return dist;
    }
    
    virtual QString getUnit() const { return "Hz"; }

protected:
    float m_dist;
    float m_s2;
};

int
SpectrogramLayer::getVerticalZoomSteps(int &defaultStep) const
{
    if (!m_model) return 0;

    int sr = m_model->getSampleRate();

    SpectrogramRangeMapper mapper(sr, m_fftSize);

//    int maxStep = mapper.getPositionForValue((float(sr) / m_fftSize) + 0.001);
    int maxStep = mapper.getPositionForValue(0);
    int minStep = mapper.getPositionForValue(float(sr) / 2);

    size_t initialMax = m_initialMaxFrequency;
    if (initialMax == 0) initialMax = sr / 2;

    defaultStep = mapper.getPositionForValue(initialMax) - minStep;

//    std::cerr << "SpectrogramLayer::getVerticalZoomSteps: " << maxStep - minStep << " (" << maxStep <<"-" << minStep << "), default is " << defaultStep << " (from initial max freq " << initialMax << ")" << std::endl;

    return maxStep - minStep;
}

int
SpectrogramLayer::getCurrentVerticalZoomStep() const
{
    if (!m_model) return 0;

    float dmin, dmax;
    getDisplayExtents(dmin, dmax);
    
    SpectrogramRangeMapper mapper(m_model->getSampleRate(), m_fftSize);
    int n = mapper.getPositionForValue(dmax - dmin);
//    std::cerr << "SpectrogramLayer::getCurrentVerticalZoomStep: " << n << std::endl;
    return n;
}

void
SpectrogramLayer::setVerticalZoomStep(int step)
{
    if (!m_model) return;

    float dmin = m_minFrequency, dmax = m_maxFrequency;
//    getDisplayExtents(dmin, dmax);

//    std::cerr << "current range " << dmin << " -> " << dmax << ", range " << dmax-dmin << ", mid " << (dmax + dmin)/2 << std::endl;
    
    int sr = m_model->getSampleRate();
    SpectrogramRangeMapper mapper(sr, m_fftSize);
    float newdist = mapper.getValueForPosition(step);

    float newmin, newmax;

    if (m_frequencyScale == LogFrequencyScale) {

        // need to pick newmin and newmax such that
        //
        // (log(newmin) + log(newmax)) / 2 == logmid
        // and
        // newmax - newmin = newdist
        //
        // so log(newmax - newdist) + log(newmax) == 2logmid
        // log(newmax(newmax - newdist)) == 2logmid
        // newmax.newmax - newmax.newdist == exp(2logmid)
        // newmax^2 + (-newdist)newmax + -exp(2logmid) == 0
        // quadratic with a = 1, b = -newdist, c = -exp(2logmid), all known
        // 
        // positive root
        // newmax = (newdist + sqrt(newdist^2 + 4exp(2logmid))) / 2
        //
        // but logmid = (log(dmin) + log(dmax)) / 2
        // so exp(2logmid) = exp(log(dmin) + log(dmax))
        // = exp(log(dmin.dmax))
        // = dmin.dmax
        // so newmax = (newdist + sqrtf(newdist^2 + 4dmin.dmax)) / 2

        newmax = (newdist + sqrtf(newdist*newdist + 4*dmin*dmax)) / 2;
        newmin = newmax - newdist;

//        std::cerr << "newmin = " << newmin << ", newmax = " << newmax << std::endl;

    } else {
        float dmid = (dmax + dmin) / 2;
        newmin = dmid - newdist / 2;
        newmax = dmid + newdist / 2;
    }

    float mmin, mmax;
    mmin = 0;
    mmax = float(sr) / 2;
    
    if (newmin < mmin) {
        newmax += (mmin - newmin);
        newmin = mmin;
    }
    if (newmax > mmax) {
        newmax = mmax;
    }
    
//    std::cerr << "SpectrogramLayer::setVerticalZoomStep: " << step << ": " << newmin << " -> " << newmax << " (range " << newdist << ")" << std::endl;

    setMinFrequency(lrintf(newmin));
    setMaxFrequency(lrintf(newmax));
}

RangeMapper *
SpectrogramLayer::getNewVerticalZoomRangeMapper() const
{
    if (!m_model) return 0;
    return new SpectrogramRangeMapper(m_model->getSampleRate(), m_fftSize);
}

void
SpectrogramLayer::updateMeasureRectYCoords(View *v, const MeasureRect &r) const
{
    int y0 = 0;
    if (r.startY > 0.0) y0 = getYForFrequency(v, r.startY);
    
    int y1 = y0;
    if (r.endY > 0.0) y1 = getYForFrequency(v, r.endY);

//    std::cerr << "SpectrogramLayer::updateMeasureRectYCoords: start " << r.startY << " -> " << y0 << ", end " << r.endY << " -> " << y1 << std::endl;

    r.pixrect = QRect(r.pixrect.x(), y0, r.pixrect.width(), y1 - y0);
}

void
SpectrogramLayer::setMeasureRectYCoord(View *v, MeasureRect &r, bool start, int y) const
{
    if (start) {
        r.startY = getFrequencyForY(v, y);
        r.endY = r.startY;
    } else {
        r.endY = getFrequencyForY(v, y);
    }
//    std::cerr << "SpectrogramLayer::setMeasureRectYCoord: start " << r.startY << " <- " << y << ", end " << r.endY << " <- " << y << std::endl;

}

void
SpectrogramLayer::toXml(QTextStream &stream,
                        QString indent, QString extraAttributes) const
{
    QString s;
    
    s += QString("channel=\"%1\" "
		 "windowSize=\"%2\" "
		 "windowHopLevel=\"%3\" "
		 "gain=\"%4\" "
		 "threshold=\"%5\" ")
	.arg(m_channel)
	.arg(m_windowSize)
	.arg(m_windowHopLevel)
	.arg(m_gain)
	.arg(m_threshold);

    s += QString("minFrequency=\"%1\" "
		 "maxFrequency=\"%2\" "
		 "colourScale=\"%3\" "
		 "colourScheme=\"%4\" "
		 "colourRotation=\"%5\" "
		 "frequencyScale=\"%6\" "
		 "binDisplay=\"%7\" "
		 "normalizeColumns=\"%8\" "
                 "normalizeVisibleArea=\"%9\"")
	.arg(m_minFrequency)
	.arg(m_maxFrequency)
	.arg(m_colourScale)
	.arg(m_colourMap)
	.arg(m_colourRotation)
	.arg(m_frequencyScale)
	.arg(m_binDisplay)
	.arg(m_normalizeColumns ? "true" : "false")
        .arg(m_normalizeVisibleArea ? "true" : "false");

    Layer::toXml(stream, indent, extraAttributes + " " + s);
}

void
SpectrogramLayer::setProperties(const QXmlAttributes &attributes)
{
    bool ok = false;

    int channel = attributes.value("channel").toInt(&ok);
    if (ok) setChannel(channel);

    size_t windowSize = attributes.value("windowSize").toUInt(&ok);
    if (ok) setWindowSize(windowSize);

    size_t windowHopLevel = attributes.value("windowHopLevel").toUInt(&ok);
    if (ok) setWindowHopLevel(windowHopLevel);
    else {
        size_t windowOverlap = attributes.value("windowOverlap").toUInt(&ok);
        // a percentage value
        if (ok) {
            if (windowOverlap == 0) setWindowHopLevel(0);
            else if (windowOverlap == 25) setWindowHopLevel(1);
            else if (windowOverlap == 50) setWindowHopLevel(2);
            else if (windowOverlap == 75) setWindowHopLevel(3);
            else if (windowOverlap == 90) setWindowHopLevel(4);
        }
    }

    float gain = attributes.value("gain").toFloat(&ok);
    if (ok) setGain(gain);

    float threshold = attributes.value("threshold").toFloat(&ok);
    if (ok) setThreshold(threshold);

    size_t minFrequency = attributes.value("minFrequency").toUInt(&ok);
    if (ok) {
        std::cerr << "SpectrogramLayer::setProperties: setting min freq to " << minFrequency << std::endl;
        setMinFrequency(minFrequency);
    }

    size_t maxFrequency = attributes.value("maxFrequency").toUInt(&ok);
    if (ok) {
        std::cerr << "SpectrogramLayer::setProperties: setting max freq to " << maxFrequency << std::endl;
        setMaxFrequency(maxFrequency);
    }

    ColourScale colourScale = (ColourScale)
	attributes.value("colourScale").toInt(&ok);
    if (ok) setColourScale(colourScale);

    int colourMap = attributes.value("colourScheme").toInt(&ok);
    if (ok) setColourMap(colourMap);

    int colourRotation = attributes.value("colourRotation").toInt(&ok);
    if (ok) setColourRotation(colourRotation);

    FrequencyScale frequencyScale = (FrequencyScale)
	attributes.value("frequencyScale").toInt(&ok);
    if (ok) setFrequencyScale(frequencyScale);

    BinDisplay binDisplay = (BinDisplay)
	attributes.value("binDisplay").toInt(&ok);
    if (ok) setBinDisplay(binDisplay);

    bool normalizeColumns =
	(attributes.value("normalizeColumns").trimmed() == "true");
    setNormalizeColumns(normalizeColumns);

    bool normalizeVisibleArea =
	(attributes.value("normalizeVisibleArea").trimmed() == "true");
    setNormalizeVisibleArea(normalizeVisibleArea);
}
    
