/*
 * This file is part of the Falcon Player (FPP) and is Copyright (C)
 * 2013-2022 by the Falcon Player Developers.
 *
 * The Falcon Player (FPP) is free software, and is covered under
 * multiple Open Source licenses.  Please see the included 'LICENSES'
 * file for descriptions of what files are covered by each license.
 *
 * This source file is covered under the GPL v2 as described in the
 * included LICENSE.GPL file.
 */

#include "fpp-pch.h"

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>

#include "spixels.h"

#include "Plugin.h"
class SpixelsPlugin : public FPPPlugins::Plugin, public FPPPlugins::ChannelOutputPlugin {
public:
    SpixelsPlugin() :
        FPPPlugins::Plugin("Spixels") {
    }
    virtual ChannelOutput* createChannelOutput(unsigned int startChannel, unsigned int channelCount) override {
        return new SpixelsOutput(startChannel, channelCount);
    }
};

extern "C" {
FPPPlugins::Plugin* createPlugin() {
    return new SpixelsPlugin();
}
}

/////////////////////////////////////////////////////////////////////////////

/*
 *
 */
SpixelsOutput::SpixelsOutput(unsigned int startChannel, unsigned int channelCount) :
    ThreadedChannelOutput(startChannel, channelCount),
    m_spi(NULL) {
    LogDebug(VB_CHANNELOUT, "SpixelsOutput::SpixelsOutput(%u, %u)\n",
             startChannel, channelCount);
}

/*
 *
 */
SpixelsOutput::~SpixelsOutput() {
    LogDebug(VB_CHANNELOUT, "SpixelsOutput::~SpixelsOutput()\n");

    for (int s = 0; s < m_strips.size(); s++)
        delete m_strips[s];

    delete m_spi;

    for (int s = 0; s < m_strings.size(); s++)
        delete m_strings[s];
}

/*
 *
 */
int SpixelsOutput::Init(Json::Value config) {
    LogDebug(VB_CHANNELOUT, "SpixelsOutput::Init(JSON)\n");

    bool haveWS2801 = false;

    for (int i = 0; i < config["outputs"].size(); i++) {
        Json::Value s = config["outputs"][i];

        if (s["protocol"].asString() == "ws2801")
            haveWS2801 = true;
    }

#if 0
// Can't use ws2801 DMA for now until mailbox issue is resolved.
// spixels includes its own copy of the mailbox code, but
// ends up calling the copy from jgarff's rpi-ws281x library
// since the functions have the same names.  Do we fork and
// rename or patch and submit a pull request?
// WS2801 is disabled in the UI, but code is left here for
// debugging.

	if (haveWS2801)
		m_spi = CreateDMAMultiSPI(); // WS2801 needs DMA for accurate timing
	else
#endif
    m_spi = CreateDirectMultiSPI();

    for (int i = 0; i < config["outputs"].size(); i++) {
        Json::Value s = config["outputs"][i];
        PixelString* newString = new PixelString();
        LEDStrip* strip = NULL;

        if (!newString->Init(s))
            return 0;

        int connector = 0;
        int pixels = newString->m_outputChannels / 3; // FIXME, need to confirm this

        if (pixels == 0) {
            delete newString;
            continue;
        }

        switch (s["portNumber"].asInt()) {
        case 0:
            connector = MultiSPI::SPI_P1;
            break;
        case 1:
            connector = MultiSPI::SPI_P2;
            break;
        case 2:
            connector = MultiSPI::SPI_P3;
            break;
        case 3:
            connector = MultiSPI::SPI_P4;
            break;
        case 4:
            connector = MultiSPI::SPI_P5;
            break;
        case 5:
            connector = MultiSPI::SPI_P6;
            break;
        case 6:
            connector = MultiSPI::SPI_P7;
            break;
        case 7:
            connector = MultiSPI::SPI_P8;
            break;
        case 8:
            connector = MultiSPI::SPI_P9;
            break;
        case 9:
            connector = MultiSPI::SPI_P10;
            break;
        case 10:
            connector = MultiSPI::SPI_P11;
            break;
        case 11:
            connector = MultiSPI::SPI_P12;
            break;
        case 12:
            connector = MultiSPI::SPI_P13;
            break;
        case 13:
            connector = MultiSPI::SPI_P14;
            break;
        case 14:
            connector = MultiSPI::SPI_P15;
            break;
        case 15:
            connector = MultiSPI::SPI_P16;
            break;
        }

        std::string protocol = s["protocol"].asString();
        toLower(protocol);
        if (protocol == "ws2801") {
            strip = CreateWS2801Strip(m_spi, connector, pixels);
        } else if (protocol == "apa102") {
            strip = CreateAPA102Strip(m_spi, connector, pixels);
        } else if (protocol == "lpd6803") {
            strip = CreateLPD6803Strip(m_spi, connector, pixels);
        } else if (protocol == "lpd8806") {
            strip = CreateLPD8806Strip(m_spi, connector, pixels);
        } else {
            LogErr(VB_CHANNELOUT, "Unknown Pixel Protocol: %s\n", s["protocol"].asString().c_str());
            return 0;
        }

        m_strings.push_back(newString);
        m_strips.push_back(strip);
    }

    LogDebug(VB_CHANNELOUT, "   Found %d strings of pixels\n", m_strings.size());
    PixelString::AutoCreateOverlayModels(m_strings);
    return ThreadedChannelOutput::Init(config);
}

/*
 *
 */
int SpixelsOutput::Close(void) {
    LogDebug(VB_CHANNELOUT, "SpixelsOutput::Close()\n");

    return ThreadedChannelOutput::Close();
}

void SpixelsOutput::GetRequiredChannelRanges(const std::function<void(int, int)>& addRange) {
    PixelString* ps = NULL;
    for (int s = 0; s < m_strings.size(); s++) {
        ps = m_strings[s];
        int min = FPPD_MAX_CHANNELS;
        int max = 0;
        int inCh = 0;
        for (int p = 0; p < ps->m_outputChannels; p++) {
            int ch = ps->m_outputMap[inCh++];
            if (ch < FPPD_MAX_CHANNELS) {
                min = std::min(min, ch);
                max = std::max(max, ch);
            }
        }
        if (min < max) {
            addRange(min, max);
        }
    }
}
void SpixelsOutput::PrepData(unsigned char* channelData) {
    unsigned char* c = channelData;
    unsigned int r = 0;
    unsigned int g = 0;
    unsigned int b = 0;

    PixelString* ps = NULL;
    int inCh = 0;

    for (int s = 0; s < m_strings.size(); s++) {
        ps = m_strings[s];
        inCh = 0;

        for (int p = 0, pix = 0; p < ps->m_outputChannels; pix++) {
            r = ps->m_brightnessMaps[p++][channelData[ps->m_outputMap[inCh++]]];
            g = ps->m_brightnessMaps[p++][channelData[ps->m_outputMap[inCh++]]];
            b = ps->m_brightnessMaps[p++][channelData[ps->m_outputMap[inCh++]]];

            m_strips[s]->SetPixel(pix, RGBc(r, g, b));
        }
    }
}

/*
 *
 */
int SpixelsOutput::RawSendData(unsigned char* channelData) {
    LogExcess(VB_CHANNELOUT, "SpixelsOutput::RawSendData(%p)\n", channelData);

    if (m_spi)
        m_spi->SendBuffers();

    return m_channelCount;
}

/*
 *
 */
void SpixelsOutput::DumpConfig(void) {
    LogDebug(VB_CHANNELOUT, "SpixelsOutput::DumpConfig()\n");

    for (int i = 0; i < m_strings.size(); i++) {
        LogDebug(VB_CHANNELOUT, "    String #%d\n", i);
        m_strings[i]->DumpConfig();
    }

    ThreadedChannelOutput::DumpConfig();
}
