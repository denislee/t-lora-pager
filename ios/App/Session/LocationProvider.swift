import CoreLocation
import Foundation
import PagerKit

/// One-shot CoreLocation fix plus a reverse-geocoded city, for the weather
/// `"auto"` key (IOS_COMPANION_PLAN.md §5.5). "When In Use" only; when the
/// user declines, `place()` throws and WeatherService falls back to IP
/// geolocation.
@MainActor
final class LocationProvider: NSObject, CLLocationManagerDelegate {
    enum Failure: Error {
        case denied
        case timeout
        case failed(String)
    }

    private let manager = CLLocationManager()
    private var waiters: [CheckedContinuation<CLLocation, Error>] = []
    private var authWaiters: [CheckedContinuation<Void, Never>] = []
    private var lastFix: CLLocation?

    override init() {
        super.init()
        manager.delegate = self
        manager.desiredAccuracy = kCLLocationAccuracyKilometer
    }

    /// Ask for "When In Use" once, so the prompt shows while the user is
    /// looking at the app rather than when the pager asks later.
    func requestAuthorizationIfNeeded() {
        if manager.authorizationStatus == .notDetermined {
            manager.requestWhenInUseAuthorization()
        }
    }

    /// Current place; reuses a fix younger than 15 minutes.
    func place() async throws -> WeatherPlace {
        let location = try await fix()
        var city = ""
        if let mark = try? await CLGeocoder().reverseGeocodeLocation(location).first {
            city = mark.locality ?? mark.subAdministrativeArea ?? mark.administrativeArea ?? ""
        }
        return WeatherPlace(lat: location.coordinate.latitude,
                            lon: location.coordinate.longitude,
                            city: city)
    }

    private func fix() async throws -> CLLocation {
        if let lastFix, -lastFix.timestamp.timeIntervalSinceNow < 15 * 60 {
            return lastFix
        }
        if manager.authorizationStatus == .notDetermined {
            await withCheckedContinuation { authWaiters.append($0) }
        }
        switch manager.authorizationStatus {
        case .authorizedWhenInUse, .authorizedAlways: break
        default: throw Failure.denied
        }
        return try await withCheckedThrowingContinuation { cont in
            waiters.append(cont)
            if waiters.count == 1 {
                manager.requestLocation()
                Task { [weak self] in
                    try? await Task.sleep(for: .seconds(8))
                    self?.finish(.failure(Failure.timeout))
                }
            }
        }
    }

    private func finish(_ result: Result<CLLocation, Error>) {
        let pending = waiters
        waiters.removeAll()
        for w in pending { w.resume(with: result) }
    }

    nonisolated func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard let location = locations.last else { return }
        Task { @MainActor in
            self.lastFix = location
            self.finish(.success(location))
        }
    }

    nonisolated func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        let message = error.localizedDescription
        Task { @MainActor in self.finish(.failure(Failure.failed(message))) }
    }

    nonisolated func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        Task { @MainActor in
            guard self.manager.authorizationStatus != .notDetermined else { return }
            let pending = self.authWaiters
            self.authWaiters.removeAll()
            for w in pending { w.resume() }
        }
    }
}
