// Verify using the public key embedded in the app, independently of the signer.
import CryptoKit
import Foundation

do {
    let arguments = CommandLine.arguments
    guard arguments.count == 4,
          let publicBytes = Data(base64Encoded: arguments[1]), publicBytes.count == 32,
          let signature = Data(base64Encoded: arguments[2]), signature.count == 64 else {
        throw NSError(domain: "LlavonUpdate", code: 1,
                      userInfo: [NSLocalizedDescriptionKey: "usage: verify-update-signature <public-key> <signature> <file>"])
    }
    let key = try Curve25519.Signing.PublicKey(rawRepresentation: publicBytes)
    let data = try Data(contentsOf: URL(fileURLWithPath: arguments[3]), options: .mappedIfSafe)
    guard key.isValidSignature(signature, for: data) else {
        throw NSError(domain: "LlavonUpdate", code: 2,
                      userInfo: [NSLocalizedDescriptionKey: "update signature does not match the app's public key"])
    }
} catch {
    FileHandle.standardError.write(Data("\(error.localizedDescription)\n".utf8))
    exit(1)
}
