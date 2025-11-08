# Path to your crates.json file
$cratesFile = "c:\fragmount(1)\fragmount\website\inventory\json\csgo-api\crates.json"
$outputFile = "c:\fragmount(1)\fragmount\case_definitions.txt"

# Read the JSON file
$jsonContent = Get-Content -Path $cratesFile -Raw | ConvertFrom-Json

$output = @()

# Add items section header
$output += "`"items`""
$output += "{"

# Process each crate
foreach ($crate in $jsonContent) {
    if ($crate.id -and $crate.id.StartsWith("crate-")) {
        $crateId = $crate.id -replace 'crate-', ''
        $crateName = $crate.name
        
        $output += "`t`"$crateId`""
        $output += "`t{"
        $output += "`t`t`"name`"`t`t`"weapon_case_$crateId`""
        $output += "`t`t`"prefab`"`t`"weapon_case_base`""
        $output += "`t`t`"item_class`"`t`"supply_crate`""
        $output += "`t`t`"item_type_name`"`t`"#CSGO_Type_WeaponCase`""
        $output += "`t`t`"item_quality`"`t`"unique`""
        $output += "`t`t`"item_rarity`"`t`"common`""
        $output += "`t`t`"item_slot`"`t`"crate`""
        $output += "`t`t`"item_name`"`t`"#CSGO_case_$crateId`""
        $output += "`t`t`"item_description`"`t`"#CSGO_case_${crateId}_desc`""
        $output += "`t`t`"image_inventory`"`t`"econ/weapon_case/weapon_case_$crateId`""
        $output += "`t`t`"model_player`"`t`"models/weapons/w_weapon_case.mdl`""
        $output += "`t`t`"capabilities`""
        $output += "`t`t{"
        $output += "`t`t`t`"nameable`"`t`"0`""
        $output += "`t`t}"
        $output += "`t}"
        $output += ""
    }
}

# Close items section
$output += "}"

# Save to file
$output | Out-File -FilePath $outputFile -Encoding utf8
Write-Host "Case definitions generated in: $outputFile"