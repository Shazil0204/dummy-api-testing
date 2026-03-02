using Microsoft.AspNetCore.Mvc;

namespace dummy_api_testing
{
    public class RelocateRequest
    {
        public List<string> AssetBarcodes { get; set; } = [];
        public string DestinationBarcode { get; set; } = "";
    }

    [ApiController]
    [Route("api/[controller]")]
    public class AssetController : ControllerBase
    {
        private static readonly List<string> _dummyBarcodes = [.. Enumerable.Range(1, 1000).Select(x => x.ToString())];
        private static readonly List<string> _dummyAssetBarcodes = [.. Enumerable.Range(1, 50).Select(x => x.ToString())];
        private static readonly List<string> _dummyLocationBarcodes = [.. Enumerable.Range(51, 1000).Select(x => x.ToString())];

        [HttpGet("validate/{barcode}")]
        public ActionResult<bool> ValidateBarcode(string barcode)
        {
            bool response = _dummyBarcodes.Contains(barcode);
            return response;
        }

        [HttpPost("relocate")]
        public ActionResult<bool> Relocate([FromBody] RelocateRequest request)
        {
            // Check if all asset barcodes are valid (1-50)
            bool allAssetsValid = request.AssetBarcodes.All(b => _dummyAssetBarcodes.Contains(b));

            // Check if destination barcode is a valid location (51-100)
            bool locationValid = _dummyLocationBarcodes.Contains(request.DestinationBarcode);

            bool response = (allAssetsValid && locationValid);
            return response;
        }
    }
}
