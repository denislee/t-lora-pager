import CoreLocation
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
    /// The city reverse-geocoded for `lastFix` (P7.62: once per fix, not per call).
    private var lastCity: (fix: CLLocation, city: String)?
    /// Bumped per location request; a timer only fires for its own request.
    private var fixRequest = 0
    private var fixTimer: Task<Void, Never>?

    /// Budget against the pager's 15 s `WEATHER_GET_TIMEOUT_MS`: fix 4 s +
    /// geocode 3 s + forecast 5 s; on a failed fix, 4 s + IP lookup 5 s +
    /// forecast 5 s.
    static let fixTimeout: Duration = .seconds(4)
    static let geocodeTimeout: Duration = .seconds(3)

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

    /// Current place; reuses a fix younger than 15 minutes and its city.
    func place() async throws -> WeatherPlace {
        let location = try await fix()
        let city: String
        if let lastCity, lastCity.fix === location {
            city = lastCity.city
        } else {
            city = await reverseGeocode(location)
            if !city.isEmpty { lastCity = (location, city) }
        }
        return WeatherPlace(lat: location.coordinate.latitude,
                            lon: location.coordinate.longitude,
                            city: city)
    }

    /// The fix's city, or "" when the geocoder fails or takes longer than
    /// `geocodeTimeout` (cancelling makes it complete with an error).
    private func reverseGeocode(_ location: CLLocation) async -> String {
        let geocoder = CLGeocoder()
        let timer = Task { @MainActor in
            try? await Task.sleep(for: Self.geocodeTimeout)
            if !Task.isCancelled { geocoder.cancelGeocode() }
        }
        defer { timer.cancel() }
        guard let mark = try? await geocoder.reverseGeocodeLocation(location).first else { return "" }
        return mark.locality ?? mark.subAdministrativeArea ?? mark.administrativeArea ?? ""
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
                fixRequest += 1
                let request = fixRequest
                manager.requestLocation()
                fixTimer = Task { [weak self] in
                    try? await Task.sleep(for: Self.fixTimeout)
                    guard !Task.isCancelled, let self, self.fixRequest == request else { return }
                    self.finish(.failure(Failure.timeout))
                }
            }
        }
    }

    private func finish(_ result: Result<CLLocation, Error>) {
        fixTimer?.cancel()
        fixTimer = nil
        fixRequest += 1
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
