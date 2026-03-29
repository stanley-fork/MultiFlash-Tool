# Disclaimer

SakuraEDL is a low-level service and research tool intended for legitimate device maintenance, recovery, testing, and development workflows.

By using this project, you acknowledge and agree to the following:

## No Warranty

This software is provided on an "as is" and "as available" basis, without warranties of any kind, express or implied, including but not limited to merchantability, fitness for a particular purpose, non-infringement, reliability, or data safety.

## Use At Your Own Risk

Operations performed through EDL, Firehose, partition flashing, vendor authentication, boot configuration, or similar low-level mechanisms may:

- permanently brick a device
- erase user data
- invalidate warranty or support status
- trigger anti-rollback, anti-tamper, or security protections
- leave a device in an unrecoverable or partially recoverable state

You are solely responsible for verifying device compatibility, loader compatibility, storage type, target partition selection, and the legality of the operation you perform.

## Legal and Compliance Responsibility

You must use this software only on hardware and data you own or are explicitly authorized to service, inspect, recover, or modify.

You are solely responsible for compliance with:

- local law and regulation
- device manufacturer terms and restrictions
- export control rules
- data protection and privacy requirements
- internal service or organizational policy

## Third-Party Services and Credentials

Some vendor authentication paths may depend on third-party services, locally supplied credentials, or private signing material. This repository does not grant any right to use such services or credentials, and public source availability does not imply authorization to access them.

## Limitation of Liability

To the maximum extent permitted by applicable law, the authors, contributors, maintainers, and distributors of this project shall not be liable for any direct, indirect, incidental, special, exemplary, punitive, or consequential damages, including but not limited to device damage, data loss, business interruption, service denial, or legal claims arising from the use or misuse of this software.

## Sensitive Operations

If you do not fully understand the effect of a low-level operation, do not run it on a real device.

Always:

- back up critical partitions before writing
- verify target model and storage layout
- confirm the origin and trust level of loaders and firmware files
- test on non-critical hardware when possible

If you do not accept these terms, do not use SakuraEDL.
