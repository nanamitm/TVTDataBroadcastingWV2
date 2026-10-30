// Compatibility fixes for web-bml 7832397's AIT wire decoder.
// application_identifier is organization_id (32 bits), application_id (16 bits).
// See ARIB STD-B23 Part 2 Table 10-6 / ETSI ES 201812 application_identifier.
import { parseAITSection as upstreamParse, SectionAssembler } from "../../web-bml/dist/server/ait.js";
export { SectionAssembler };

export function parseAITSection(bytes: Uint8Array) {
    if (bytes.length < 16 || bytes.length > 4096 || bytes.length !== 3 + (((bytes[1] & 15) << 8) | bytes[2])) return undefined;
    let crc = 0xffffffff;
    for (const byte of bytes) {
        crc ^= byte << 24;
        for (let bit = 0; bit < 8; bit++) crc = ((crc << 1) ^ ((crc & 0x80000000) ? 0x04c11db7 : 0)) >>> 0;
    }
    if (crc !== 0) return undefined;
    try {
        const section = upstreamParse(bytes);
        if (!section || section.sectionNumber > section.lastSectionNumber) return undefined;
        return { ...section, applications: section.applications.map(app => ({ ...app,
            organizationId: (app.organizationId * 0x10000 + Math.floor(app.applicationId / 0x10000)) >>> 0,
            applicationId: app.applicationId & 0xffff,
        })) };
    } catch {
        return undefined;
    }
}
