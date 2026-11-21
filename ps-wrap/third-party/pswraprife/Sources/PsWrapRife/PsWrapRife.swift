import Foundation
import CoreVideo
import RifeMetal

private final class Box { let interp: RifeInterpolator; init(_ i: RifeInterpolator) { interp = i } }

@_cdecl("pswrap_rife_create")
public func pswrap_rife_create(_ path: UnsafePointer<CChar>?, _ tier: Int32) -> UnsafeMutableRawPointer? {
    guard let path = path else { return nil }
    let t: RifeQualityTier = tier == 0 ? .hq : (tier == 1 ? .balanced : .fast)
    do {
        let cfg = RifeConfiguration(modelURL: URL(fileURLWithPath: String(cString: path)), qualityTier: t)
        let i = try RifeInterpolator(configuration: cfg)
        return Unmanaged.passRetained(Box(i)).toOpaque()
    } catch {
        FileHandle.standardError.write("pswrap_rife_create failed: \(error)\n".data(using: .utf8)!)
        return nil
    }
}

@_cdecl("pswrap_rife_interpolate")
public func pswrap_rife_interpolate(_ h: UnsafeMutableRawPointer?, _ prev: CVPixelBuffer, _ cur: CVPixelBuffer,
                                    _ out: CVPixelBuffer, _ t: Float) -> Int32 {
    guard let h = h else { return -1 }
    let box = Unmanaged<Box>.fromOpaque(h).takeUnretainedValue()
    do {
        _ = t
        try box.interp.interpolate(previous: prev, current: cur, output: out)
        return 0
    } catch {
        FileHandle.standardError.write("pswrap_rife_interpolate failed: \(error)\n".data(using: .utf8)!)
        return -2
    }
}

@_cdecl("pswrap_rife_destroy")
public func pswrap_rife_destroy(_ h: UnsafeMutableRawPointer?) {
    guard let h = h else { return }
    Unmanaged<Box>.fromOpaque(h).release()
}
